#include "user/gfx.h"

/**
 * bounce - animated balls in a window, ~30 frames per second (bonus: window compositor).
 * Space adds a ball, Q or the close box quits. Run it next to `paint` / `gclock`: every
 * window is its own process with its own buffer, composed by the kernel.
 */

#define MAX_BALLS 24

struct Ball {
    int32_t x, y, vx, vy, r;  // position in 1/16 pixel
    uint32_t color;
};

static uint32_t seed;
static uint32_t rnd(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static void add_ball(struct Ball *b, int32_t w, int32_t h) {
    b->r = 8 + (int32_t) (rnd() % 14);
    b->x = (b->r + (int32_t) (rnd() % (uint32_t) (w - 2 * b->r))) * 16;
    b->y = (b->r + (int32_t) (rnd() % (uint32_t) (h / 2))) * 16;
    b->vx = (int32_t) (rnd() % 80) - 40;
    b->vy = (int32_t) (rnd() % 40) - 20;
    b->color = GFX_RGB(80 + rnd() % 176, 80 + rnd() % 176, 80 + rnd() % 176);
}

int main(int argc, char *argv[]) {
    if (!gfx_available()) {
        printf("bounce: needs the graphics mode (the window compositor is off in VGA text mode)\n");
        return 1;
    }
    uint32_t count = 6;
    if (argc > 1)
        parse_uint(argv[1], &count);
    if (count > MAX_BALLS)
        count = MAX_BALLS;
    struct Canvas c;
    if (!canvas_open(&c, 360, 260, "bounce", -1, -1)) {
        printf("bounce: cannot open a window\n");
        return 1;
    }
    printf("bounce: window opened (pid %u)\n", getpid());
    detach();
    seed = uptime_ms() * 2654435761u + getpid();
    struct Ball balls[MAX_BALLS];
    for (uint32_t i = 0; i < count; i++)
        add_ball(&balls[i], c.w, c.h);
    uint32_t frames = 0, fps = 0, second = uptime_ms();
    char title[32];
    for (;;) {
        struct SyscallWindowEvent e;
        for (canvas_event(&c, &e, false); e.type != SYSCALL_WEVENT_NONE; canvas_event(&c, &e, false)) {
            if (e.type == SYSCALL_WEVENT_CLOSE || (e.type == SYSCALL_WEVENT_KEY && (e.key == 'q' || e.key == 'Q'))) {
                canvas_close(&c);
                return 0;
            }
            if (e.type == SYSCALL_WEVENT_KEY && e.key == ' ' && count < MAX_BALLS)
                add_ball(&balls[count++], c.w, c.h);
        }
        // simulate: gravity, walls with a little energy loss
        for (uint32_t i = 0; i < count; i++) {
            struct Ball *b = &balls[i];
            b->vy += 3;
            b->x += b->vx;
            b->y += b->vy;
            if (b->x < b->r * 16) { b->x = b->r * 16; b->vx = -b->vx; }
            if (b->x > (c.w - b->r) * 16) { b->x = (c.w - b->r) * 16; b->vx = -b->vx; }
            if (b->y > (c.h - b->r) * 16) { b->y = (c.h - b->r) * 16; b->vy = -b->vy * 15 / 16; if (b->vy > -40) b->vy = -110; }
            if (b->y < b->r * 16) { b->y = b->r * 16; b->vy = -b->vy; }
        }
        // draw
        for (int32_t y = 0; y < c.h; y++) // vertical gradient background
            gfx_rect(&c, 0, y, c.w, 1, GFX_RGB(10, 10 + y / 8, 30 + y / 4));
        for (uint32_t i = 0; i < count; i++) {
            gfx_circle(&c, balls[i].x / 16, balls[i].y / 16, balls[i].r, balls[i].color);
            gfx_circle(&c, balls[i].x / 16 - balls[i].r / 3, balls[i].y / 16 - balls[i].r / 3, balls[i].r / 4,
                       GFX_RGB(255, 255, 255));
        }
        snprintf(title, sizeof(title), "%u balls  %u fps", count, fps);
        gfx_text(&c, 6, 4, title, GFX_RGB(230, 230, 230), 1);
        canvas_present(&c);
        frames++;
        if (uptime_ms() - second >= 1000) {
            fps = frames;
            frames = 0;
            second = uptime_ms();
        }
        sleep_ms(30);
    }
}
