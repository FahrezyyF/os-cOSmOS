#include "user/gfx.h"

/**
 * gclock - analog clock in a window (CMOS RTC through SYS_TIME). CLOCK_TZ shifts the
 * hours (export CLOCK_TZ=7 for WIB). Q or the close box quits.
 */

static const int16_t sin60[60] = {
    0, 107, 213, 316, 416, 512, 602, 685, 761, 828, 887, 935, 974, 1002, 1018, 1024, 1018, 1002, 974, 935,
    887, 828, 761, 685, 602, 512, 416, 316, 213, 107, 0, -107, -213, -316, -416, -512, -602, -685, -761, -828,
    -887, -935, -974, -1002, -1018, -1024, -1018, -1002, -974, -935, -887, -828, -761, -685, -602, -512, -416, -316, -213, -107,
};

static void hand(struct Canvas *c, int32_t cx, int32_t cy, int32_t pos60, int32_t len, int32_t width, uint32_t rgb) {
    int32_t s = sin60[pos60 % 60], co = sin60[(pos60 + 15) % 60];
    gfx_thick_line(c, cx, cy, cx + s * len / 1024, cy - co * len / 1024, width, rgb);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    if (!gfx_available()) {
        printf("gclock: needs the graphics mode (the window compositor is off in VGA text mode)\n");
        return 1;
    }
    struct Canvas c;
    if (!canvas_open(&c, 220, 240, "gclock", -1, -1)) {
        printf("gclock: cannot open a window\n");
        return 1;
    }
    printf("gclock: window opened (pid %u)\n", getpid());
    detach();
    const char *tz = getenv("CLOCK_TZ");
    int32_t offset = tz ? atoi(tz) : 0;
    int32_t cx = c.w / 2, cy = 110, r = 96;
    uint8_t last_second = 255;
    for (;;) {
        struct SyscallWindowEvent e;
        for (canvas_event(&c, &e, false); e.type != SYSCALL_WEVENT_NONE; canvas_event(&c, &e, false))
            if (e.type == SYSCALL_WEVENT_CLOSE || (e.type == SYSCALL_WEVENT_KEY && (e.key == 'q' || e.key == 'Q'))) {
                canvas_close(&c);
                return 0;
            }
        struct SyscallTime t;
        get_time(&t);
        if (t.second != last_second) {
            last_second = t.second;
            int32_t hour = ((int32_t) t.hour + offset % 24 + 24) % 24;
            gfx_clear(&c, GFX_RGB(24, 26, 34));
            gfx_circle(&c, cx, cy, r, GFX_RGB(236, 236, 228));
            gfx_ring(&c, cx, cy, r, GFX_RGB(60, 60, 70));
            for (int32_t k = 0; k < 60; k++) {
                int32_t inner = k % 5 == 0 ? r - 12 : r - 5;
                int32_t s = sin60[k], co = sin60[(k + 15) % 60];
                gfx_line(&c, cx + s * inner / 1024, cy - co * inner / 1024, cx + s * (r - 2) / 1024,
                         cy - co * (r - 2) / 1024, GFX_RGB(40, 40, 50));
            }
            hand(&c, cx, cy, (hour % 12) * 5 + t.minute / 12, r * 5 / 10, 3, GFX_RGB(30, 30, 40));
            hand(&c, cx, cy, t.minute, r * 8 / 10, 2, GFX_RGB(30, 30, 40));
            hand(&c, cx, cy, t.second, r * 9 / 10, 0, GFX_RGB(220, 40, 40));
            gfx_circle(&c, cx, cy, 4, GFX_RGB(220, 40, 40));
            char text[32];
            snprintf(text, sizeof(text), "%02d:%02u:%02u", (int) hour, t.minute, t.second);
            gfx_text(&c, cx - 32, cy + r + 8, text, GFX_RGB(230, 230, 230), 1);
            canvas_present(&c);
        }
        sleep_ms(200);
    }
}
