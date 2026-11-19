#include "user/gfx.h"

/**
 * vplay - video player for CVID files (tools/video2cvid.py), e.g. Bad Apple!!.
 *
 *   vplay [-t] [-l] [-s scale] <file.cvid>
 *
 * Graphics mode: the video plays in a window (scaled, default x3); the close box, Q or
 * Ctrl+C stop it. VGA text mode (or -t): ASCII art in the console; Ctrl+C stops it.
 * The whole file is read from the file system first, then frames are decoded on the fly
 * (XOR deltas stored as run lengths) and shown on time: the player sleeps until each
 * frame is due and skips drawing when it falls behind, so the speed stays right.
 */

struct VideoHeader {
    char     magic[4];
    uint16_t version, width, height, fps;
    uint32_t frames, flags;
    uint8_t  reserved[12];
} __attribute__((packed));

static uint32_t get_varint(const uint8_t **p, const uint8_t *end) {
    uint32_t v = 0, shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        v |= (uint32_t) (b & 0x7F) << shift;
        if (!(b & 0x80))
            break;
        shift += 7;
    }
    return v;
}

// Apply one frame (XOR runs) to the bitmap
static void decode(uint8_t *pixels, uint32_t count, const uint8_t *p, const uint8_t *end) {
    uint32_t pos = 0;
    bool changed = false;
    while (pos < count && p < end) {
        uint32_t run = get_varint(&p, end);
        if (changed)
            for (uint32_t i = 0; i < run && pos + i < count; i++)
                pixels[pos + i] ^= 1;
        pos += run;
        changed = !changed;
    }
}

// Absolute path or relative to the working directory -> parent inode + leaf name
static bool locate(const char *path, uint32_t *parent, const char **leaf) {
    char full[512];
    if (path[0] == '/') {
        strncpy(full, path, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    } else {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd)))
            return false;
        snprintf(full, sizeof(full), "%s/%s", strcmp(cwd, "/") == 0 ? "" : cwd, path);
    }
    uint32_t dir = FS_ROOT;
    char *p = full;
    for (;;) {
        while (*p == '/')
            p++;
        char *slash = strchr(p, '/');
        if (slash == NULL)
            break;
        struct SyscallStat st;
        if (fs_stat(dir, p, (uint8_t) (slash - p), &st) != 0 || st.type != SYSCALL_TYPE_DIR)
            return false;
        dir = st.inode;
        p = slash + 1;
    }
    *parent = dir;
    *leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    return true;
}

int main(int argc, char *argv[]) {
    bool text = false, loop = false;
    uint32_t scale = 3;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0)
            text = true;
        else if (strcmp(argv[i], "-l") == 0)
            loop = true;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            parse_uint(argv[++i], &scale);
        else
            path = argv[i];
    }
    if (path == NULL) {
        printf("usage: vplay [-t] [-l] [-s scale] <file.cvid>   (try: vplay /media/demo.cvid)\n");
        return 2;
    }
    uint32_t parent;
    const char *leaf;
    char *data;
    uint32_t size;
    if (!locate(path, &parent, &leaf) || fs_read_file(parent, leaf, (uint8_t) strlen(leaf), &data, &size) != 0) {
        printf("vplay: cannot read %s\n", path);
        return 1;
    }
    struct VideoHeader h;
    if (size < sizeof(h) || memcmp(data, "CVID", 4) != 0) {
        printf("vplay: %s is not a CVID video (convert with tools/video2cvid.py)\n", path);
        return 1;
    }
    memcpy(&h, data, sizeof(h));
    uint32_t count = (uint32_t) h.width * h.height;
    uint8_t *pixels = calloc(count, 1);
    if (pixels == NULL || h.fps == 0 || h.width == 0 || h.height == 0) {
        printf("vplay: bad video header\n");
        return 1;
    }
    if (scale < 1)
        scale = 1;
    if (!text && !gfx_available())
        text = true;
    struct Canvas c;
    if (!text) {
        while (scale > 1 && (h.width * scale > 800 || h.height * scale > 600))
            scale--;
        if (!canvas_open(&c, (int32_t) (h.width * scale), (int32_t) (h.height * scale), "vplay", -1, -1)) {
            printf("vplay: cannot open a window, using the console\n");
            text = true;
        }
    }
    struct SyscallScreenInfo screen;
    console_screen_info(&screen);
    uint32_t cols = screen.cols, rows = screen.rows - 1;
    char *line = malloc(cols + 1);
    printf("vplay: %s, %ux%u, %u fps, %u frames (%u s)%s\n", path, h.width, h.height, h.fps, h.frames,
           h.frames / h.fps, text ? "" : " - close the window, Q or Ctrl+C to stop");
    if (text)
        console_clear_screen();

    uint32_t shown = 0, skipped = 0, fps_count = 0, fps = 0;
    do {
        memset(pixels, 0, count);
        const uint8_t *p = (const uint8_t*) data + sizeof(h), *end = (const uint8_t*) data + size;
        uint32_t start = uptime_ms(), second = start;
        for (uint32_t f = 0; f < h.frames && p + 4 <= end; f++) {
            uint32_t len;
            memcpy(&len, p, 4);
            p += 4;
            if (p + len > end)
                break;
            decode(pixels, count, p, p + len);
            p += len;
            uint32_t due = start + f * 1000 / h.fps, now = uptime_ms();
            if (now > due + 1000 / h.fps) { // late by more than a frame: decode only
                skipped++;
                continue;
            }
            if (!text) {
                for (uint32_t y = 0; y < h.height; y++) {
                    for (uint32_t x = 0; x < h.width; x++) {
                        uint32_t rgb = pixels[y * h.width + x] ? 0xFFFFFF : 0x000000;
                        uint32_t *dst = &c.px[(y * scale) * (uint32_t) c.w + x * scale];
                        for (uint32_t dy = 0; dy < scale; dy++, dst += c.w)
                            for (uint32_t dx = 0; dx < scale; dx++)
                                dst[dx] = rgb;
                    }
                }
                canvas_present(&c);
                struct SyscallWindowEvent e;
                for (canvas_event(&c, &e, false); e.type != SYSCALL_WEVENT_NONE; canvas_event(&c, &e, false))
                    if (e.type == SYSCALL_WEVENT_CLOSE ||
                        (e.type == SYSCALL_WEVENT_KEY && (e.key == 'q' || e.key == 'Q' || e.key == 3))) {
                        canvas_close(&c);
                        printf("vplay: stopped at frame %u (%u shown, %u skipped)\n", f, shown, skipped);
                        return 0;
                    }
            } else {
                for (uint32_t r = 0; r < rows; r++) {
                    uint32_t y = r * h.height / rows;
                    for (uint32_t col = 0; col < cols; col++)
                        line[col] = pixels[y * h.width + col * h.width / cols] ? '#' : ' ';
                    console_set_cursor((uint8_t) r, 0);
                    console_raw_write(line, cols, C_WHITE);
                }
            }
            shown++;
            fps_count++;
            if (uptime_ms() - second >= 1000) {
                fps = fps_count;
                fps_count = 0;
                second = uptime_ms();
                if (!text) {
                    char title[32];
                    snprintf(title, sizeof(title), "vplay %u fps", fps);
                    canvas_title(&c, title);
                }
            }
            now = uptime_ms();
            if (now < due)
                sleep_ms(due - now); // the process sleeps: other programs keep running
        }
    } while (loop);
    if (!text)
        canvas_close(&c);
    else
        console_clear_screen();
    printf("vplay: done, %u frames shown, %u skipped, last second %u fps\n", shown, skipped, fps);
    return 0;
}
