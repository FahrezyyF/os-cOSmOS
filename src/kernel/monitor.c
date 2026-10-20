#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "header/kernel/monitor.h"
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/graphics/graphics.h"
#include "header/driver/keyboard.h"
#include "header/driver/timer.h"
#include "header/driver/mouse.h"
#include "header/driver/rtc.h"
#include "header/driver/speaker.h"
#include "header/driver/power.h"
#include "header/driver/disk.h"
#include "header/filesystem/vfs.h"
#include "header/stdlib/string.h"
#include "header/stdlib/format.h"

#define LINE_MAX        200
#define MAX_ARGS        16
#define HISTORY_SIZE    16
#define IO_BUFFER_SIZE  (96 * 1024)
#define LIST_MAX        300
#define TREE_DEPTH      5
#define TREE_WIDTH      40
#define PATH_MAX        256

static enum VFSKind active_fs = VFS_EXT2;
static uint32_t     cwd[VFS_COUNT];
static char         history[HISTORY_SIZE][LINE_MAX + 1];
static uint32_t     history_count = 0;
static uint8_t      io_buffer[IO_BUFFER_SIZE];
static uint8_t      pattern_buffer[80 * 1024];
static struct VFSEntry list_buffer[LIST_MAX];
static struct VFSEntry tree_buffer[TREE_DEPTH][TREE_WIDTH];

static struct MouseState mouse_view;
static int32_t           wheel_total = 0;
static uint32_t          last_status_ms = 0;
static bool              status_dirty = true;

/* ================================ OUTPUT HELPERS ==================================== */

static void cprintf(uint8_t fg, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cprintf(uint8_t fg, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uint8_t old_fg, old_bg;
    console_get_color(&old_fg, &old_bg);
    console_set_color(fg, old_bg);
    console_puts(buf);
    console_set_color(old_fg, old_bg);
}

#define ERROR(...) cprintf(COLOR_LIGHT_RED, __VA_ARGS__)
#define OK(...)    cprintf(COLOR_LIGHT_GREEN, __VA_ARGS__)

static void build_path(enum VFSKind kind, uint32_t dir, char *out, uint32_t cap) {
    static char names[16][VFS_NAME_MAX + 1];
    uint32_t depth = 0;
    bool truncated = false;
    while (dir != vfs_root(kind)) {
        uint32_t parent;
        if (depth == 16 || !vfs_name_of(kind, dir, &parent, names[depth])) {
            truncated = depth == 16;
            break;
        }
        depth++;
        dir = parent;
    }
    uint32_t len = 0;
    out[0] = '\0';
    if (truncated)
        len += (uint32_t) snprintf(out + len, cap - len, "/...");
    if (depth == 0 && !truncated) {
        snprintf(out, cap, "/");
        return;
    }
    while (depth > 0 && len < cap)
        len += (uint32_t) snprintf(out + len, cap - len, "/%s", names[--depth]);
}

/* ================================ STATUS BAR ======================================== */

static void draw_status_bar(void) {
    uint8_t cols = framebuffer_get_cols();
    char path[PATH_MAX], left[PATH_MAX + 64], right[96];
    build_path(active_fs, cwd[active_fs], path, sizeof(path));
    struct RTCTime t;
    rtc_read(&t);
    snprintf(left, sizeof(left), " cOSmOS v0.2.0 | %s:%s ", vfs_name(active_fs), path);
    if (cols < 100) // 80-column text mode: compact clock only
        snprintf(right, sizeof(right), " %02u:%02u:%02u UTC ", t.hour, t.minute, t.second);
    else if (mouse_is_present())
        snprintf(right, sizeof(right), " mouse %d,%d [%c%c%c] wheel %d | %04u-%02u-%02u %02u:%02u:%02u UTC ",
                 (int) mouse_view.x, (int) mouse_view.y,
                 (mouse_view.buttons & MOUSE_BUTTON_LEFT) ? 'L' : '-',
                 (mouse_view.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-',
                 (mouse_view.buttons & MOUSE_BUTTON_RIGHT) ? 'R' : '-', (int) wheel_total,
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
    else
        snprintf(right, sizeof(right), " %04u-%02u-%02u %02u:%02u:%02u UTC ", t.year, t.month, t.day, t.hour, t.minute, t.second);
    uint32_t llen = (uint32_t) strlen(left), rlen = (uint32_t) strlen(right);
    for (uint8_t c = 0; c < cols; c++) {
        char ch = ' ';
        uint8_t bg = COLOR_BLUE;
        if (c < llen) {
            ch = left[c];
            bg = COLOR_CYAN;
        } else if (c + rlen >= cols && c - (cols - rlen) < rlen) {
            ch = right[c - (cols - rlen)];
        }
        framebuffer_write(0, c, ch, COLOR_WHITE, bg);
    }
}

// Background work while waiting for a key: cursor blink, mouse pointer, status bar clock
static void idle(void) {
    uint32_t now = timer_get_ms();
    framebuffer_cursor_tick(now);
    if (mouse_is_present()) {
        struct MouseState m;
        mouse_get_state(&m);
        if (m.changed) {
            wheel_total += m.wheel;
            mouse_view = m;
            framebuffer_pointer_move(m.x, m.y);
            status_dirty = true;
        }
    }
    if (status_dirty || now - last_status_ms >= 1000) {
        draw_status_bar();
        last_status_ms = now;
        status_dirty   = false;
    }
}

/* ================================ LINE EDITOR ======================================= */

static void print_prompt(void) {
    char path[PATH_MAX];
    build_path(active_fs, cwd[active_fs], path, sizeof(path));
    cprintf(COLOR_YELLOW, "[%s] ", vfs_name(active_fs));
    cprintf(COLOR_LIGHT_BLUE, "%s", path);
    cprintf(COLOR_WHITE, " $ ");
}

static void erase_input(uint32_t len) {
    while (len-- > 0)
        console_putchar('\b');
}

// Returns false when the line was cancelled with Ctrl+C
static bool read_line(char *buf, uint32_t cap) {
    uint32_t len = 0;
    uint32_t browse = history_count;
    buf[0] = '\0';
    for (;;) {
        char c;
        get_keyboard_buffer(&c);
        if (c == 0) {
            idle();
            __asm__ volatile("hlt");
            continue;
        }
        if (c == '\n') {
            console_putchar('\n');
            buf[len] = '\0';
            return true;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                console_putchar('\b');
            }
        } else if (c == 0x03) { // Ctrl+C
            cprintf(COLOR_DARK_GRAY, "^C\n");
            buf[0] = '\0';
            return false;
        } else if (c == 0x0C) { // Ctrl+L
            console_clear();
            print_prompt();
            buf[len] = '\0';
            console_puts(buf);
        } else if (c == KEY_ARROW_UP || c == KEY_ARROW_DOWN) {
            uint32_t oldest = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
            if (c == KEY_ARROW_UP && browse > oldest)
                browse--;
            else if (c == KEY_ARROW_DOWN && browse < history_count)
                browse++;
            else
                continue;
            erase_input(len);
            if (browse == history_count) {
                len = 0;
            } else {
                strncpy(buf, history[browse % HISTORY_SIZE], cap - 1);
                buf[cap - 1] = '\0';
                len = (uint32_t) strlen(buf);
            }
            buf[len] = '\0';
            console_puts(buf);
        } else if (c >= 0x20 && c < 0x7F && len + 1 < cap) {
            buf[len++] = c;
            console_putchar(c);
        }
    }
}

static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < MAX_ARGS) {
        while (*p == ' ')
            p++;
        if (*p == '\0')
            break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p != '\0' && *p != '"')
                p++;
        } else {
            argv[argc++] = p;
            while (*p != '\0' && *p != ' ')
                p++;
        }
        if (*p != '\0')
            *p++ = '\0';
    }
    return argc;
}

/* ================================ PATHS ============================================= */

/**
 * Split a path into its parent directory and last component.
 * "a/b/c" -> (dir of a/b, "c"); "/", "a/" or "" -> (dir, empty leaf)
 */
static bool resolve_parent(const char *path, uint32_t *dir_out, char *leaf, uint8_t *leaf_len, bool quiet) {
    uint32_t dir = path[0] == '/' ? vfs_root(active_fs) : cwd[active_fs];
    const char *p = path;
    *leaf_len = 0;
    leaf[0]   = '\0';
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        uint32_t n = 0;
        while (p[n] != '\0' && p[n] != '/')
            n++;
        if (n > VFS_NAME_MAX) {
            if (!quiet) ERROR("name too long\n");
            return false;
        }
        bool trailing = p[n] == '/';
        const char *rest = p + n;
        while (*rest == '/')
            rest++;
        if (*rest == '\0' && !trailing) {
            memcpy(leaf, p, n);
            leaf[n]   = '\0';
            *leaf_len = (uint8_t) n;
            break;
        }
        struct VFSEntry e;
        if (vfs_lookup(active_fs, dir, p, (uint8_t) n, &e) != 0) {
            if (!quiet) ERROR("no such directory: %.*s\n", (int) n, p);
            return false;
        }
        if (e.type != VFS_TYPE_DIR) {
            if (!quiet) ERROR("not a directory: %.*s\n", (int) n, p);
            return false;
        }
        dir = e.id;
        p   = rest;
    }
    *dir_out = dir;
    return true;
}

static bool resolve_dir(const char *path, uint32_t *out, bool quiet) {
    uint32_t dir;
    char leaf[VFS_NAME_MAX + 1];
    uint8_t len;
    if (!resolve_parent(path, &dir, leaf, &len, quiet))
        return false;
    if (len == 0) {
        *out = dir;
        return true;
    }
    struct VFSEntry e;
    if (vfs_lookup(active_fs, dir, leaf, len, &e) != 0) {
        if (!quiet) ERROR("no such directory: %s\n", path);
        return false;
    }
    if (e.type != VFS_TYPE_DIR) {
        if (!quiet) ERROR("not a directory: %s\n", path);
        return false;
    }
    *out = e.id;
    return true;
}

static bool require_fs(void) {
    if (vfs_available(active_fs))
        return true;
    ERROR("%s volume is not mounted (is the disk image attached?)\n", vfs_name(active_fs));
    return false;
}

static const char *write_error(int8_t code) {
    switch (code) {
        case 1:  return "already exists";
        case 2:  return "invalid parent folder";
        default: return "invalid name or not enough free space";
    }
}

static const char *read_error(int8_t code) {
    switch (code) {
        case 1:  return "is a directory";
        case 2:  return "file is larger than the monitor buffer (96 KiB)";
        case 3:  return "no such file";
        case 4:  return "invalid parent folder";
        default: return "I/O error";
    }
}

static const char *delete_error(int8_t code) {
    switch (code) {
        case 1:  return "no such file or directory";
        case 2:  return "directory not empty";
        case 3:  return "invalid parent folder";
        default: return "cannot delete this entry";
    }
}

static const char *rename_error(int8_t code) {
    switch (code) {
        case 1:  return "source not found";
        case 2:  return "destination already exists";
        case 3:  return "invalid folder";
        case 4:  return "cannot move a folder into itself";
        default: return "invalid name or not enough free space";
    }
}

static void sort_entries(struct VFSEntry *entries, int32_t count) {
    for (int32_t i = 1; i < count; i++) {
        struct VFSEntry key = entries[i];
        int32_t j = i - 1;
        while (j >= 0 && (entries[j].type < key.type || (entries[j].type == key.type && strcmp(entries[j].name, key.name) > 0))) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

/* ================================ COMMANDS ========================================== */

static void cmd_help(int argc, char **argv);

static void cmd_ls(int argc, char **argv) {
    if (!require_fs())
        return;
    bool long_format = false;
    const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-la") == 0)
            long_format = true;
        else
            path = argv[i];
    }
    uint32_t dir;
    if (!resolve_dir(path, &dir, false))
        return;
    int32_t n = vfs_list(active_fs, dir, list_buffer, LIST_MAX);
    if (n < 0) {
        ERROR("cannot list directory (error %d)\n", (int) n + 10);
        return;
    }
    sort_entries(list_buffer + 2, n - 2); // keep "." and ".." first
    uint32_t files = 0, dirs = 0, bytes = 0;
    for (int32_t i = 0; i < n; i++) {
        struct VFSEntry *e = &list_buffer[i];
        bool dot = i < 2;
        if (dot && !long_format)
            continue;
        if (long_format) {
            struct VFSStat st;
            vfs_stat(active_fs, e->id, &st);
            struct RTCTime t;
            rtc_from_unix(st.mtime, &t);
            cprintf(COLOR_DARK_GRAY, "  %5u ", e->id);
            cprintf(COLOR_LIGHT_GRAY, "%c %8u B %4u blk  %04u-%02u-%02u %02u:%02u  ",
                    e->type == VFS_TYPE_DIR ? 'd' : '-', e->size, st.blocks, t.year, t.month, t.day, t.hour, t.minute);
        } else {
            console_puts("  ");
        }
        if (e->type == VFS_TYPE_DIR)
            cprintf(COLOR_LIGHT_BLUE, "%s/\n", e->name);
        else if (long_format)
            cprintf(COLOR_WHITE, "%s\n", e->name);
        else
            cprintf(COLOR_WHITE, "%-32s %8u B\n", e->name, e->size);
        if (!dot) {
            if (e->type == VFS_TYPE_DIR) dirs++;
            else { files++; bytes += e->size; }
        }
    }
    cprintf(COLOR_DARK_GRAY, "  %u director%s, %u file%s, %u bytes\n", dirs, dirs == 1 ? "y" : "ies", files, files == 1 ? "" : "s", bytes);
}

static void cmd_cd(int argc, char **argv) {
    if (!require_fs())
        return;
    uint32_t dir = vfs_root(active_fs);
    if (argc >= 2 && !resolve_dir(argv[1], &dir, false))
        return;
    cwd[active_fs] = dir;
    status_dirty   = true;
}

static void cmd_pwd(int argc, char **argv) {
    (void) argc;
    (void) argv;
    char path[PATH_MAX];
    build_path(active_fs, cwd[active_fs], path, sizeof(path));
    kprintf("%s:%s\n", vfs_name(active_fs), path);
}

static bool split_target(const char *path, uint32_t *dir, char *leaf, uint8_t *len) {
    if (!resolve_parent(path, dir, leaf, len, false))
        return false;
    if (*len == 0) {
        ERROR("missing file name in '%s'\n", path);
        return false;
    }
    return true;
}

static void cmd_mkdir(int argc, char **argv) {
    if (!require_fs())
        return;
    for (int i = 1; i < argc; i++) {
        uint32_t dir;
        char leaf[VFS_NAME_MAX + 1];
        uint8_t len;
        if (!split_target(argv[i], &dir, leaf, &len))
            continue;
        int8_t ret = vfs_write(active_fs, dir, leaf, len, NULL, 0, true);
        if (ret != 0)
            ERROR("mkdir %s: %s\n", argv[i], write_error(ret));
    }
}

static void cmd_touch(int argc, char **argv) {
    if (!require_fs())
        return;
    for (int i = 1; i < argc; i++) {
        uint32_t dir;
        char leaf[VFS_NAME_MAX + 1];
        uint8_t len;
        if (!split_target(argv[i], &dir, leaf, &len))
            continue;
        int8_t ret = vfs_write(active_fs, dir, leaf, len, "", 0, false);
        if (ret != 0)
            ERROR("touch %s: %s\n", argv[i], write_error(ret));
    }
}

static void cmd_write(int argc, char **argv) {
    if (!require_fs())
        return;
    if (argc < 3) {
        ERROR("usage: write <file> <text...>   (\\n inserts a newline)\n");
        return;
    }
    uint32_t len = 0;
    for (int i = 2; i < argc; i++) {
        for (const char *p = argv[i]; *p != '\0' && len < IO_BUFFER_SIZE - 2; p++) {
            if (p[0] == '\\' && p[1] == 'n') {
                io_buffer[len++] = '\n';
                p++;
            } else {
                io_buffer[len++] = (uint8_t) *p;
            }
        }
        if (i + 1 < argc)
            io_buffer[len++] = ' ';
    }
    io_buffer[len++] = '\n';
    uint32_t dir;
    char leaf[VFS_NAME_MAX + 1];
    uint8_t nlen;
    if (!split_target(argv[1], &dir, leaf, &nlen))
        return;
    int8_t ret = vfs_write(active_fs, dir, leaf, nlen, io_buffer, len, false);
    if (ret != 0)
        ERROR("write %s: %s\n", argv[1], write_error(ret));
    else
        OK("wrote %u bytes to %s\n", len, argv[1]);
}

static void cmd_cat(int argc, char **argv) {
    if (!require_fs())
        return;
    for (int i = 1; i < argc; i++) {
        uint32_t dir;
        char leaf[VFS_NAME_MAX + 1];
        uint8_t len;
        if (!split_target(argv[i], &dir, leaf, &len))
            continue;
        struct VFSEntry e;
        if (vfs_lookup(active_fs, dir, leaf, len, &e) != 0) {
            ERROR("cat %s: no such file\n", argv[i]);
            continue;
        }
        int8_t ret = vfs_read(active_fs, dir, leaf, len, io_buffer, IO_BUFFER_SIZE);
        if (ret != 0) {
            ERROR("cat %s: %s\n", argv[i], read_error(ret));
            continue;
        }
        for (uint32_t k = 0; k < e.size; k++) {
            char c = (char) io_buffer[k];
            console_putchar((c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) ? c : '.');
        }
        if (e.size > 0 && io_buffer[e.size - 1] != '\n')
            console_putchar('\n');
    }
}

static void cmd_rm(int argc, char **argv) {
    if (!require_fs())
        return;
    for (int i = 1; i < argc; i++) {
        uint32_t dir;
        char leaf[VFS_NAME_MAX + 1];
        uint8_t len;
        if (!split_target(argv[i], &dir, leaf, &len))
            continue;
        int8_t ret = vfs_delete(active_fs, dir, leaf, len);
        if (ret != 0)
            ERROR("rm %s: %s\n", argv[i], delete_error(ret));
    }
}

// Destination of mv / cp: an existing directory keeps the source name
static bool resolve_destination(const char *dst, const char *src_name, uint8_t src_len,
                                uint32_t *dir, char *leaf, uint8_t *len) {
    uint32_t existing;
    if (resolve_dir(dst, &existing, true)) {
        *dir = existing;
        memcpy(leaf, src_name, src_len);
        leaf[src_len] = '\0';
        *len = src_len;
        return true;
    }
    return split_target(dst, dir, leaf, len);
}

static void cmd_mv(int argc, char **argv) {
    if (!require_fs())
        return;
    if (argc != 3) {
        ERROR("usage: mv <source> <destination>\n");
        return;
    }
    uint32_t sdir, ddir;
    char sleaf[VFS_NAME_MAX + 1], dleaf[VFS_NAME_MAX + 1];
    uint8_t slen, dlen;
    if (!split_target(argv[1], &sdir, sleaf, &slen) || !resolve_destination(argv[2], sleaf, slen, &ddir, dleaf, &dlen))
        return;
    int8_t ret = vfs_rename(active_fs, sdir, sleaf, slen, ddir, dleaf, dlen);
    if (ret != 0)
        ERROR("mv: %s\n", rename_error(ret));
}

static void cmd_cp(int argc, char **argv) {
    if (!require_fs())
        return;
    if (argc != 3) {
        ERROR("usage: cp <file> <destination>\n");
        return;
    }
    uint32_t sdir, ddir;
    char sleaf[VFS_NAME_MAX + 1], dleaf[VFS_NAME_MAX + 1];
    uint8_t slen, dlen;
    if (!split_target(argv[1], &sdir, sleaf, &slen))
        return;
    struct VFSEntry e;
    if (vfs_lookup(active_fs, sdir, sleaf, slen, &e) != 0) {
        ERROR("cp: no such file: %s\n", argv[1]);
        return;
    }
    int8_t ret = vfs_read(active_fs, sdir, sleaf, slen, io_buffer, IO_BUFFER_SIZE);
    if (ret != 0) {
        ERROR("cp: %s\n", read_error(ret));
        return;
    }
    if (!resolve_destination(argv[2], sleaf, slen, &ddir, dleaf, &dlen))
        return;
    ret = vfs_write(active_fs, ddir, dleaf, dlen, io_buffer, e.size, false);
    if (ret != 0)
        ERROR("cp: %s\n", write_error(ret));
}

static void cmd_find(int argc, char **argv) {
    if (!require_fs())
        return;
    if (argc != 2 || strlen(argv[1]) > VFS_NAME_MAX) {
        ERROR("usage: find <exact name>\n");
        return;
    }
    static struct VFSFindHit hits[64];
    uint32_t start = timer_get_ms();
    uint32_t total = vfs_find(active_fs, argv[1], (uint8_t) strlen(argv[1]), hits, 64);
    uint32_t took  = timer_get_ms() - start;
    for (uint32_t i = 0; i < total && i < 64; i++) {
        char path[PATH_MAX];
        build_path(active_fs, hits[i].id, path, sizeof(path));
        if (hits[i].type == VFS_TYPE_DIR)
            cprintf(COLOR_LIGHT_BLUE, "  %s/\n", path);
        else
            cprintf(COLOR_WHITE, "  %s\n", path);
    }
    cprintf(COLOR_DARK_GRAY, "  %u match%s via the %s name index (%u ms, no tree walk)\n", total, total == 1 ? "" : "es",
            active_fs == VFS_EXT2 ? "in-memory" : "on-disk", took);
}

static void tree_walk(uint32_t dir, uint32_t depth, char *prefix, uint32_t *dirs, uint32_t *files) {
    if (depth >= TREE_DEPTH)
        return;
    int32_t n = vfs_list(active_fs, dir, tree_buffer[depth], TREE_WIDTH);
    if (n <= 2)
        return;
    sort_entries(tree_buffer[depth] + 2, n - 2);
    for (int32_t i = 2; i < n; i++) {
        struct VFSEntry *e = &tree_buffer[depth][i];
        bool last = i == n - 1;
        cprintf(COLOR_DARK_GRAY, "%s%c%c%c ", prefix, last ? (char) 0xC0 : (char) 0xC3, (char) 0xC4, (char) 0xC4);
        if (e->type == VFS_TYPE_DIR) {
            (*dirs)++;
            cprintf(COLOR_LIGHT_BLUE, "%s/\n", e->name);
            uint32_t plen = (uint32_t) strlen(prefix);
            if (plen + 5 < 64) {
                prefix[plen]     = last ? ' ' : (char) 0xB3;
                prefix[plen + 1] = ' ';
                prefix[plen + 2] = ' ';
                prefix[plen + 3] = ' ';
                prefix[plen + 4] = '\0';
                tree_walk(e->id, depth + 1, prefix, dirs, files);
                prefix[plen] = '\0';
            }
        } else {
            (*files)++;
            cprintf(COLOR_WHITE, "%s", e->name);
            cprintf(COLOR_DARK_GRAY, " (%u B)\n", e->size);
        }
    }
}

static void cmd_tree(int argc, char **argv) {
    if (!require_fs())
        return;
    uint32_t dir;
    if (!resolve_dir(argc >= 2 ? argv[1] : ".", &dir, false))
        return;
    char path[PATH_MAX], prefix[64] = "";
    build_path(active_fs, dir, path, sizeof(path));
    cprintf(COLOR_LIGHT_BLUE, "%s\n", path);
    uint32_t dirs = 0, files = 0;
    tree_walk(dir, 0, prefix, &dirs, &files);
    cprintf(COLOR_DARK_GRAY, "%u directories, %u files\n", dirs, files);
}

static void print_time(const char *label, uint32_t timestamp) {
    struct RTCTime t;
    rtc_from_unix(timestamp, &t);
    kprintf("  %-9s: %04u-%02u-%02u %02u:%02u:%02u UTC\n", label, t.year, t.month, t.day, t.hour, t.minute, t.second);
}

static void cmd_stat(int argc, char **argv) {
    if (!require_fs())
        return;
    if (argc != 2) {
        ERROR("usage: stat <path>\n");
        return;
    }
    uint32_t dir;
    char leaf[VFS_NAME_MAX + 1];
    uint8_t len;
    if (!resolve_parent(argv[1], &dir, leaf, &len, false))
        return;
    struct VFSEntry e;
    uint32_t id = dir;
    if (len > 0) {
        if (vfs_lookup(active_fs, dir, leaf, len, &e) != 0) {
            ERROR("stat: no such file or directory\n");
            return;
        }
        id = e.id;
    }
    struct VFSStat st;
    if (!vfs_stat(active_fs, id, &st)) {
        ERROR("stat: cannot read metadata\n");
        return;
    }
    kprintf("  %-9s: %s\n", "path", argv[1]);
    kprintf("  %-9s: %u (%s)\n", active_fs == VFS_EXT2 ? "inode" : "node", id, st.type == VFS_TYPE_DIR ? "directory" : "regular file");
    kprintf("  %-9s: %u bytes\n", "size", st.size);
    if (active_fs == VFS_EXT2)
        kprintf("  %-9s: %u (512-byte, incl. indirect)  links: %u\n", "blocks", st.blocks, st.links);
    else
        kprintf("  %-9s: %u data blocks in %u extent%s\n", "blocks", st.blocks, st.extents, st.extents == 1 ? "" : "s");
    print_time("created", st.ctime);
    print_time("modified", st.mtime);
}

static void cmd_hexdump(int argc, char **argv) {
    if (!require_fs())
        return;
    if (argc < 2) {
        ERROR("usage: hexdump <file> [bytes]\n");
        return;
    }
    uint32_t limit = 256;
    if (argc >= 3) {
        limit = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            limit = limit * 10 + (uint32_t) (*p - '0');
    }
    uint32_t dir;
    char leaf[VFS_NAME_MAX + 1];
    uint8_t len;
    if (!split_target(argv[1], &dir, leaf, &len))
        return;
    struct VFSEntry e;
    if (vfs_lookup(active_fs, dir, leaf, len, &e) != 0) {
        ERROR("hexdump: no such file: %s\n", argv[1]);
        return;
    }
    int8_t ret = vfs_read(active_fs, dir, leaf, len, io_buffer, IO_BUFFER_SIZE);
    if (ret != 0) {
        ERROR("hexdump: %s\n", read_error(ret));
        return;
    }
    if (limit > e.size)
        limit = e.size;
    for (uint32_t off = 0; off < limit; off += 16) {
        cprintf(COLOR_DARK_GRAY, "  %08x  ", off);
        for (uint32_t k = 0; k < 16; k++) {
            if (off + k < limit)
                kprintf("%02x ", io_buffer[off + k]);
            else
                kprintf("   ");
        }
        console_puts(" |");
        for (uint32_t k = 0; k < 16 && off + k < limit; k++) {
            char c = (char) io_buffer[off + k];
            console_putchar(c >= 0x20 && c < 0x7F ? c : '.');
        }
        console_puts("|\n");
    }
}

static void cmd_df(int argc, char **argv) {
    (void) argc;
    (void) argv;
    for (int k = 0; k < VFS_COUNT; k++) {
        struct VFSStats st;
        vfs_stats((enum VFSKind) k, &st);
        cprintf(COLOR_YELLOW, "%s", vfs_name((enum VFSKind) k));
        kprintf("  %s on %s\n", st.fs_name, st.device);
        if (!vfs_available((enum VFSKind) k)) {
            ERROR("    not mounted\n");
            continue;
        }
        uint32_t used = st.total_blocks - st.free_blocks;
        kprintf("    blocks: %u / %u used (%u KiB free), nodes: %u / %u used, mounts: %u\n",
                used, st.total_blocks, st.free_blocks / 2, st.total_nodes - st.free_nodes, st.total_nodes, st.mount_count);
        if (k == VFS_COSMOFS)
            kprintf("    journal: last transaction #%u, replayed at mount: %u\n", st.journal_sequence, st.replayed);
    }
}

static void cmd_fsck(int argc, char **argv) {
    enum VFSKind kind = active_fs;
    if (argc >= 2)
        kind = strcmp(argv[1], "cosmo") == 0 ? VFS_COSMOFS : VFS_EXT2;
    char message[128];
    uint32_t errors;
    uint32_t start = timer_get_ms();
    bool ok = vfs_check(kind, &errors, message, sizeof(message));
    if (ok)
        OK("fsck %s: %s (%u ms)\n", vfs_name(kind), message, timer_get_ms() - start);
    else
        ERROR("fsck %s: %u error(s), first: %s\n", vfs_name(kind), errors, message);
}

static void cmd_use(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "ext2") != 0 && strcmp(argv[1], "cosmo") != 0)) {
        ERROR("usage: use ext2|cosmo\n");
        return;
    }
    active_fs    = strcmp(argv[1], "ext2") == 0 ? VFS_EXT2 : VFS_COSMOFS;
    status_dirty = true;
    require_fs();
}

/* -- Live CRUD self-test on the mounted disk -- */

static uint32_t selftest_passed, selftest_failed;

static void selftest_check(bool condition, const char *what, int32_t detail) {
    if (condition) {
        selftest_passed++;
        return;
    }
    selftest_failed++;
    ERROR("    FAIL: %s (%d)\n", what, (int) detail);
}

static void fill_pattern(uint8_t *buf, uint32_t size, uint32_t seed) {
    uint32_t s = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < size; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t) (s >> 16);
    }
}

static void cmd_selftest(int argc, char **argv) {
    (void) argc;
    (void) argv;
    if (!require_fs())
        return;
    static const uint32_t sizes[] = {0, 1, 511, 512, 513, 2047, 2048, 6144, 6145, 40000, 71680, 72000};
    const uint32_t count = sizeof(sizes) / sizeof(sizes[0]);
    enum VFSKind kind = active_fs;
    uint32_t root = vfs_root(kind);
    selftest_passed = selftest_failed = 0;
    uint32_t start = timer_get_ms();
    cprintf(COLOR_YELLOW, "selftest on %s: CRUD + data integrity on the live disk\n", vfs_name(kind));

    // Clean leftovers of an interrupted run
    struct VFSEntry e;
    if (vfs_lookup(kind, root, "selftest.dir", 12, &e) == 0) {
        int32_t n = vfs_list(kind, e.id, list_buffer, LIST_MAX);
        for (int32_t i = 2; i < n; i++)
            vfs_delete(kind, e.id, list_buffer[i].name, list_buffer[i].name_len);
        vfs_delete(kind, root, "selftest.dir", 12);
    }

    selftest_check(vfs_write(kind, root, "selftest.dir", 12, NULL, 0, true) == 0, "mkdir selftest.dir", 0);
    selftest_check(vfs_write(kind, root, "selftest.dir", 12, NULL, 0, true) == 1, "mkdir again -> 1", 0);
    uint32_t dir = 0;
    selftest_check(vfs_lookup(kind, root, "selftest.dir", 12, &e) == 0 && e.type == VFS_TYPE_DIR, "lookup dir", 0);
    dir = e.id;

    char name[16];
    for (uint32_t i = 0; i < count; i++) {
        snprintf(name, sizeof(name), "file%02u", i);
        uint8_t len = (uint8_t) strlen(name);
        fill_pattern(pattern_buffer, sizes[i], i + 7);
        int8_t ret = vfs_write(kind, dir, name, len, pattern_buffer, sizes[i], false);
        selftest_check(ret == 0, "write file", (int32_t) sizes[i]);
        memset(io_buffer, 0xEE, sizes[i] + 16);
        ret = vfs_read(kind, dir, name, len, io_buffer, sizes[i]);
        bool same = ret == 0 && memcmp(io_buffer, pattern_buffer, sizes[i]) == 0 && io_buffer[sizes[i]] == 0xEE;
        selftest_check(same, "read back identical bytes (size)", (int32_t) sizes[i]);
        if (sizes[i] > 0)
            selftest_check(vfs_read(kind, dir, name, len, io_buffer, sizes[i] - 1) == 2, "small buffer -> 2", (int32_t) sizes[i]);
        kprintf("    %-7s %6u bytes  %s\n", name, sizes[i], same ? "ok" : "MISMATCH");
    }
    selftest_check(vfs_write(kind, dir, "file00", 6, "x", 1, false) == 1, "duplicate name -> 1", 0);
    selftest_check(vfs_read(kind, root, "selftest.dir", 12, io_buffer, IO_BUFFER_SIZE) == 1, "read a folder -> 1", 0);
    selftest_check(vfs_read(kind, dir, "missing", 7, io_buffer, IO_BUFFER_SIZE) == 3, "read missing -> 3", 0);
    selftest_check(vfs_delete(kind, root, "selftest.dir", 12) == 2, "delete non-empty folder -> 2", 0);
    int32_t listed = vfs_list(kind, dir, list_buffer, LIST_MAX);
    selftest_check(listed == (int32_t) count + 2, "read_directory lists every file", listed);

    selftest_check(vfs_rename(kind, dir, "file01", 6, root, "selftest.moved", 14) == 0, "move file to root", 0);
    struct VFSFindHit hits[4];
    selftest_check(vfs_find(kind, "selftest.moved", 14, hits, 4) == 1 && hits[0].parent == root, "indexed find", 0);
    selftest_check(vfs_delete(kind, root, "selftest.moved", 14) == 0, "delete moved file", 0);

    for (uint32_t i = 0; i < count; i++) {
        if (i == 1)
            continue;
        snprintf(name, sizeof(name), "file%02u", i);
        selftest_check(vfs_delete(kind, dir, name, (uint8_t) strlen(name)) == 0, "delete file", (int32_t) i);
    }
    selftest_check(vfs_delete(kind, dir, "file00", 6) == 1, "delete twice -> 1", 0);
    selftest_check(vfs_delete(kind, root, "selftest.dir", 12) == 0, "delete empty folder", 0);
    char message[128];
    uint32_t errors;
    selftest_check(vfs_check(kind, &errors, message, sizeof(message)), "fsck after test", (int32_t) errors);

    uint32_t total = selftest_passed + selftest_failed;
    if (selftest_failed == 0)
        OK("selftest %s: %u/%u checks passed in %u ms\n", vfs_name(kind), selftest_passed, total, timer_get_ms() - start);
    else
        ERROR("selftest %s: %u/%u checks FAILED\n", vfs_name(kind), selftest_failed, total);
}

/* -- System commands -- */

static void cmd_time(int argc, char **argv) {
    (void) argc;
    (void) argv;
    struct RTCTime t;
    rtc_read(&t);
    uint32_t up = timer_get_ms() / 1000;
    kprintf("%04u-%02u-%02u %02u:%02u:%02u UTC (unix %u), uptime %u:%02u:%02u\n", t.year, t.month, t.day,
            t.hour, t.minute, t.second, rtc_unix_time(), up / 3600, (up / 60) % 60, up % 60);
}

static uint32_t parse_uint(const char *s, uint32_t fallback) {
    if (s == NULL || *s == '\0')
        return fallback;
    uint32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (uint32_t) (*s - '0');
    return v;
}

static void cmd_beep(int argc, char **argv) {
    uint32_t freq = parse_uint(argc > 1 ? argv[1] : NULL, 880);
    uint32_t ms   = parse_uint(argc > 2 ? argv[2] : NULL, 200);
    if (ms > 5000)
        ms = 5000;
    kprintf("beep %u Hz for %u ms (run QEMU with `make run-sound` to hear it)\n", freq, ms);
    speaker_beep(freq, ms);
}

static void cmd_mouse(int argc, char **argv) {
    (void) argc;
    (void) argv;
    if (!mouse_is_present()) {
        ERROR("no PS/2 mouse detected\n");
        return;
    }
    kprintf("PS/2 mouse (%s): position %d,%d  buttons [%c%c%c]  wheel total %d  packets %u\n",
            mouse_has_wheel() ? "IntelliMouse, wheel" : "3-button",
            (int) mouse_view.x, (int) mouse_view.y,
            (mouse_view.buttons & MOUSE_BUTTON_LEFT) ? 'L' : '-', (mouse_view.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-',
            (mouse_view.buttons & MOUSE_BUTTON_RIGHT) ? 'R' : '-', (int) wheel_total, mouse_view.packets);
    kprintf("(click inside the QEMU window to grab the mouse, Ctrl+Alt+G releases it)\n");
}

static void cmd_sysinfo(int argc, char **argv) {
    (void) argc;
    (void) argv;
    cprintf(COLOR_YELLOW, "cOSmOS v0.2.0 (Milestone 2)\n");
    if (framebuffer_is_graphics())
        kprintf("  display : BGA %ux%ux32 linear framebuffer at 0x%08x (VBE DISPI id 0x%04x), %ux%u text cells\n",
                graphics_get_width(), graphics_get_height(), graphics_get_lfb_address(), graphics_get_bga_version(),
                framebuffer_get_cols(), framebuffer_get_rows());
    else
        kprintf("  display : VGA text mode 80x25\n");
    for (uint8_t d = 0; d < 2; d++) {
        struct ATADriveInfo info;
        ata_identify(d, &info);
        if (info.present)
            kprintf("  ata %s: %s, %u sectors (%u KiB)\n", d ? "slave " : "master", info.model, info.sector_count, info.sector_count / 2);
        else
            kprintf("  ata %s: not present\n", d ? "slave " : "master");
    }
    kprintf("  timer   : PIT channel 0 at %u Hz, uptime %u ms\n", TIMER_FREQUENCY_HZ, timer_get_ms());
    kprintf("  mouse   : %s\n", mouse_is_present() ? (mouse_has_wheel() ? "PS/2 with wheel" : "PS/2") : "none");
    kprintf("  kernel  : stack 2 MiB, loaded at 1 MiB\n");
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    console_putchar('\n');
}

static void cmd_clear(int argc, char **argv) {
    (void) argc;
    (void) argv;
    console_clear();
}

static void cmd_history(int argc, char **argv) {
    (void) argc;
    (void) argv;
    uint32_t oldest = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
    for (uint32_t i = oldest; i < history_count; i++)
        kprintf("  %3u  %s\n", i + 1, history[i % HISTORY_SIZE]);
}

static void cmd_crash(int argc, char **argv) {
    (void) argc;
    (void) argv;
    cprintf(COLOR_LIGHT_RED, "dividing by zero to show the exception handler...\n");
    __asm__ volatile("xor %%ecx, %%ecx; div %%ecx" : : : "eax", "ecx", "edx");
}

static void cmd_reboot(int argc, char **argv) {
    (void) argc;
    (void) argv;
    power_reboot();
}

static void cmd_shutdown(int argc, char **argv) {
    (void) argc;
    (void) argv;
    kprintf("Flushing disks and powering off. Bye!\n");
    ata_flush(ATA_DRIVE_MASTER);
    ata_flush(ATA_DRIVE_SLAVE);
    timer_sleep_ms(300);
    power_shutdown();
}

struct MonitorCommand {
    const char *name;
    const char *usage;
    const char *help;
    void (*run)(int argc, char **argv);
};

static const struct MonitorCommand commands[] = {
    {"help",     "",                  "show this list",                                   cmd_help},
    {"ls",       "[-l] [dir]",        "list a directory",                                  cmd_ls},
    {"cd",       "[dir]",             "change directory (.., /, a/b paths)",               cmd_cd},
    {"pwd",      "",                  "print the working directory",                       cmd_pwd},
    {"mkdir",    "<dir>...",          "create directories",                                cmd_mkdir},
    {"touch",    "<file>...",         "create empty files",                                cmd_touch},
    {"write",    "<file> <text>",     "create a file containing text (\\n = newline)",     cmd_write},
    {"cat",      "<file>...",         "print files",                                       cmd_cat},
    {"cp",       "<file> <dst>",      "copy a file",                                       cmd_cp},
    {"mv",       "<src> <dst>",       "rename / move a file or folder",                    cmd_mv},
    {"rm",       "<path>...",         "delete files or empty folders",                     cmd_rm},
    {"find",     "<name>",            "indexed search by exact name",                      cmd_find},
    {"tree",     "[dir]",             "show the directory tree",                           cmd_tree},
    {"stat",     "<path>",            "inode / node metadata",                             cmd_stat},
    {"hexdump",  "<file> [bytes]",    "hex view of a file",                                cmd_hexdump},
    {"df",       "",                  "usage of both volumes",                             cmd_df},
    {"fsck",     "[ext2|cosmo]",      "file system consistency check",                     cmd_fsck},
    {"use",      "ext2|cosmo",        "switch the active volume",                          cmd_use},
    {"selftest", "",                  "CRUD + integrity test on the live disk",            cmd_selftest},
    {"time",     "",                  "CMOS clock and uptime",                             cmd_time},
    {"beep",     "[hz] [ms]",         "PC speaker",                                        cmd_beep},
    {"mouse",    "",                  "PS/2 mouse state",                                  cmd_mouse},
    {"sysinfo",  "",                  "hardware summary",                                  cmd_sysinfo},
    {"echo",     "<text>",            "print text",                                        cmd_echo},
    {"history",  "",                  "recent commands (Up / Down to recall)",             cmd_history},
    {"clear",    "",                  "clear the screen (or Ctrl+L)",                      cmd_clear},
    {"crash",    "",                  "trigger a CPU exception (panic screen demo)",       cmd_crash},
    {"reboot",   "",                  "restart the machine",                               cmd_reboot},
    {"shutdown", "",                  "power off (ACPI)",                                  cmd_shutdown},
};

static void cmd_help(int argc, char **argv) {
    (void) argc;
    (void) argv;
    for (uint32_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        cprintf(COLOR_LIGHT_CYAN, "  %-9s", commands[i].name);
        cprintf(COLOR_DARK_GRAY, "%-16s", commands[i].usage);
        kprintf("%s\n", commands[i].help);
    }
    cprintf(COLOR_DARK_GRAY, "  Up/Down: history, Ctrl+C: cancel line, Ctrl+L: clear\n");
}

static void execute(char *line) {
    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv);
    if (argc == 0)
        return;
    for (uint32_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].run(argc, argv);
            return;
        }
    }
    ERROR("%s: command not found (try `help`)\n", argv[0]);
}

void monitor_run(void) {
    for (int k = 0; k < VFS_COUNT; k++)
        cwd[k] = vfs_root((enum VFSKind) k);
    active_fs = vfs_available(VFS_EXT2) ? VFS_EXT2 : VFS_COSMOFS;

    console_set_region(1, framebuffer_get_rows());
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_clear();
    if (mouse_is_present()) {
        uint16_t w, h;
        framebuffer_get_pointer_space(&w, &h);
        mouse_view.x = w / 2;
        mouse_view.y = h / 2;
        framebuffer_pointer_move(w / 2, h / 2);
        framebuffer_pointer_set_visible(true);
    }
    draw_status_bar();

    cprintf(COLOR_LIGHT_CYAN, "Welcome to cOSmOS!");
    kprintf("  Kernel monitor for Milestone 2 - type ");
    cprintf(COLOR_YELLOW, "help");
    kprintf(" for commands, ");
    cprintf(COLOR_YELLOW, "selftest");
    kprintf(" to verify the file systems.\n");
    cmd_df(0, NULL);
    console_putchar('\n');

    char line[LINE_MAX + 1];
    for (;;) {
        print_prompt();
        if (!read_line(line, sizeof(line)) || line[0] == '\0')
            continue;
        if (history_count == 0 || strcmp(history[(history_count - 1) % HISTORY_SIZE], line) != 0) {
            strncpy(history[history_count % HISTORY_SIZE], line, LINE_MAX);
            history[history_count % HISTORY_SIZE][LINE_MAX] = '\0';
            history_count++;
        }
        execute(line);
        status_dirty = true;
    }
}
