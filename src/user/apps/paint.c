#include "user/gfx.h"

/**
 * paint - draw with the mouse in a window (bonus: window compositor).
 * Left button draws, right button erases, click a color in the palette, C clears,
 * +/- change the brush, Q or the close box quits.
 */

static const uint32_t palette[] = {
    GFX_RGB(255, 255, 255), GFX_RGB(255, 80, 80), GFX_RGB(255, 170, 40), GFX_RGB(250, 230, 60),
    GFX_RGB(90, 220, 90), GFX_RGB(70, 200, 230), GFX_RGB(90, 120, 255), GFX_RGB(200, 100, 255),
};
#define COLORS   8
#define BAR_H    24
#define CANVAS_BG GFX_RGB(16, 16, 24)

static struct Canvas c;
static int32_t color = 1, brush = 3;

static void draw_bar(void) {
    gfx_rect(&c, 0, 0, c.w, BAR_H, GFX_RGB(40, 40, 52));
    for (int32_t i = 0; i < COLORS; i++) {
        gfx_rect(&c, 4 + i * 26, 3, 22, 18, palette[i]);
        if (i == color)
            gfx_rect(&c, 4 + i * 26, 21, 22, 2, GFX_RGB(255, 255, 255));
    }
    char info[48];
    snprintf(info, sizeof(info), "brush %d  C clear  Q quit", brush);
    gfx_text(&c, 4 + COLORS * 26 + 8, 4, info, GFX_RGB(200, 200, 210), 1);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    if (!gfx_available()) {
        printf("paint: needs the graphics mode (the window compositor is off in VGA text mode)\n");
        return 1;
    }
    if (!canvas_open(&c, 480, 340, "paint", -1, -1)) {
        printf("paint: cannot open a window\n");
        return 1;
    }
    printf("paint: window opened (pid %u). Click into it to draw; the shell keeps running.\n", getpid());
    detach(); // a window program runs beside the shell
    gfx_clear(&c, CANVAS_BG);
    draw_bar();
    canvas_present(&c);
    int32_t last_x = -1, last_y = -1;
    for (;;) {
        struct SyscallWindowEvent e;
        canvas_event(&c, &e, true); // sleeps until the window gets an event
        bool dirty = false;
        do {
            if (e.type == SYSCALL_WEVENT_CLOSE || (e.type == SYSCALL_WEVENT_KEY && (e.key == 'q' || e.key == 'Q'))) {
                canvas_close(&c);
                return 0;
            }
            if (e.type == SYSCALL_WEVENT_KEY) {
                if (e.key == 'c' || e.key == 'C') {
                    gfx_rect(&c, 0, BAR_H, c.w, c.h - BAR_H, CANVAS_BG);
                } else if (e.key == '+' && brush < 20) {
                    brush++;
                } else if (e.key == '-' && brush > 1) {
                    brush--;
                } else if (e.key >= '1' && e.key <= '8') {
                    color = e.key - '1';
                }
                draw_bar();
                dirty = true;
            } else if (e.type == SYSCALL_WEVENT_MOUSE_DOWN && e.y < BAR_H) {
                int32_t i = (e.x - 4) / 26;
                if (i >= 0 && i < COLORS)
                    color = i;
                draw_bar();
                dirty = true;
            } else if ((e.type == SYSCALL_WEVENT_MOUSE_DOWN || e.type == SYSCALL_WEVENT_MOUSE_MOVE) && e.buttons) {
                uint32_t ink = (e.buttons & 2) ? CANVAS_BG : palette[color];
                int32_t y = e.y < BAR_H + brush ? BAR_H + brush : e.y;
                if (e.type == SYSCALL_WEVENT_MOUSE_MOVE && last_x >= 0)
                    gfx_thick_line(&c, last_x, last_y, e.x, y, brush, ink);
                else
                    gfx_circle(&c, e.x, y, brush, ink);
                last_x = e.x;
                last_y = y;
                dirty = true;
            } else if (e.type == SYSCALL_WEVENT_MOUSE_UP || (e.type == SYSCALL_WEVENT_MOUSE_MOVE && !e.buttons)) {
                last_x = -1;
            }
            canvas_event(&c, &e, false); // drain what is queued, then present once
        } while (e.type != SYSCALL_WEVENT_NONE);
        if (dirty)
            canvas_present(&c);
    }
}
