#include "header/kernel/splash.h"
#include "header/graphics/graphics.h"
#include "header/text/framebuffer.h"
#include "header/text/console.h"
#include "header/text/font.h"
#include "header/driver/timer.h"
#include "header/stdlib/string.h"

#define BAR_WIDTH   420
#define BAR_HEIGHT  14
#define BAR_Y       610

static uint32_t star_seed = 0x2130C0DE;

static uint32_t next_random(void) {
    star_seed = star_seed * 1103515245u + 12345u;
    return star_seed >> 8;
}

static void draw_centered_text(int y, const char *text, uint32_t color, int scale) {
    int width = (int) strlen(text) * FONT_WIDTH * scale;
    int x     = (graphics_get_width() - width) / 2;
    graphics_draw_text(x + scale, y + scale, text, RGB(0, 0, 0), scale); // drop shadow
    graphics_draw_text(x, y, text, color, scale);
}

static void draw_ring(int cx, int cy, bool front) {
    for (int t = 0; t < 7; t++) {
        uint32_t color = graphics_blend(RGB(255, 200, 120), RGB(200, 120, 255), (uint8_t) (t * 36));
        graphics_draw_ellipse(cx, cy, 190 + t * 3, 38 + t, color, front);
    }
}

static void draw_planet(int cx, int cy, int r) {
    draw_ring(cx, cy, false); // back half of the ring, hidden behind the planet below
    // Concentric circles moving towards the light source fake a shaded sphere
    for (int i = 0; i <= 10; i++) {
        int rr = r - i * r / 12;
        int ox = -i * r / 28, oy = -i * r / 28;
        uint32_t color = graphics_blend(RGB(25, 60, 150), RGB(140, 210, 255), (uint8_t) (i * 23));
        graphics_fill_circle(cx + ox, cy + oy, rr, color);
    }
    draw_ring(cx, cy, true);  // front half of the ring, drawn over the planet
}

static void graphics_splash(void) {
    int w = graphics_get_width(), h = graphics_get_height();
    for (int y = 0; y < h; y++)
        graphics_fill_rect(0, y, w, 1, graphics_blend(RGB(5, 6, 28), RGB(55, 18, 80), (uint8_t) (y * 255 / h)));
    for (int i = 0; i < 260; i++) {
        int x = (int) (next_random() % (uint32_t) w), y = (int) (next_random() % (uint32_t) h);
        if ((y > 410 && y < 580) || (y > 595 && y < 660))
            continue; // keep the title, subtitle and progress bar free of stars
        uint8_t level = (uint8_t) (120 + next_random() % 136);
        int size = (next_random() % 9 == 0) ? 2 : 1;
        graphics_fill_rect(x, y, size, size, RGB(level, level, (level + 30) > 255 ? 255 : level + 30));
    }
    draw_planet(w / 2, 270, 105);
    draw_centered_text(420, "cOSmOS", RGB(255, 255, 255), 6);
    draw_centered_text(530, "IF2130 Sistem Operasi  -  Milestone 2: File System", RGB(200, 210, 255), 1);
    draw_centered_text(552, "EXT2 IF2130 Edition  +  CosmoFS (journaled)  +  1024x768 graphics", RGB(150, 160, 220), 1);

    int bx = (w - BAR_WIDTH) / 2;
    graphics_fill_rect(bx - 2, BAR_Y - 2, BAR_WIDTH + 4, BAR_HEIGHT + 4, RGB(200, 200, 255));
    graphics_fill_rect(bx, BAR_Y, BAR_WIDTH, BAR_HEIGHT, RGB(15, 15, 40));
}

static void text_splash(void) {
    static const char *banner[] = {
        "             ____    _____            ____    _____ ",
        "      _____ / __ \\  / ___/ ____ ___  / __ \\  / ___/ ",
        "     / ___// / / /  \\__ \\ / __ `__ \\/ / / /  \\__ \\  ",
        "    / /__ / /_/ /  ___/ // / / / / / /_/ /  ___/ /  ",
        "    \\___/ \\____/  /____//_/ /_/ /_/\\____/  /____/   ",
    };
    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    console_clear();
    kprintf("\n\n\n");
    for (uint32_t i = 0; i < sizeof(banner) / sizeof(banner[0]); i++)
        kprintf("%s\n", banner[i]);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    kprintf("\n        IF2130 Sistem Operasi - Milestone 2: File System\n\n");
}

void splash_begin(void) {
    framebuffer_set_cursor_visible(false);
    if (framebuffer_is_graphics())
        graphics_splash();
    else
        text_splash();
}

void splash_step(const char *message, uint8_t percent) {
    if (percent > 100)
        percent = 100;
    if (framebuffer_is_graphics()) {
        int w  = graphics_get_width();
        int bx = (w - BAR_WIDTH) / 2;
        int filled = BAR_WIDTH * percent / 100;
        for (int x = 0; x < filled; x++)
            graphics_fill_rect(bx + x, BAR_Y, 1, BAR_HEIGHT, graphics_blend(RGB(80, 160, 255), RGB(255, 120, 220), (uint8_t) (x * 255 / BAR_WIDTH)));
        graphics_fill_rect(0, BAR_Y + 26, w, FONT_HEIGHT + 4, graphics_blend(RGB(5, 6, 28), RGB(55, 18, 80), (uint8_t) ((BAR_Y + 26) * 255 / graphics_get_height())));
        draw_centered_text(BAR_Y + 28, message, RGB(220, 220, 240), 1);
    } else {
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        kprintf("    [%3u%%] ", percent);
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        kprintf("%s\n", message);
    }
    timer_sleep_ms(120);
}

void splash_end(uint32_t hold_ms) {
    timer_sleep_ms(hold_ms);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    framebuffer_clear();
    framebuffer_set_cursor_visible(true);
}
