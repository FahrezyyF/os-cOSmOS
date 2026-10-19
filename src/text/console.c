#include <stdarg.h>
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/driver/serial.h"
#include "header/stdlib/format.h"

#define TAB_WIDTH 4

static uint8_t console_row    = 0;
static uint8_t console_col    = 0;
static uint8_t console_fg     = COLOR_LIGHT_GRAY;
static uint8_t console_bg     = COLOR_BLACK;
static uint8_t region_top     = 0;
static uint8_t region_bottom  = FRAMEBUFFER_HEIGHT;

static void console_sync_cursor(void) {
    framebuffer_set_cursor(console_row, console_col);
}

static void console_newline(void) {
    console_col = 0;
    if (console_row + 1 < region_bottom) {
        console_row++;
    } else {
        framebuffer_scroll_region(region_top, region_bottom, console_bg);
        console_row = region_bottom - 1;
    }
}

void console_init(void) {
    region_top    = 0;
    region_bottom = framebuffer_get_rows();
    console_clear();
}

void console_set_region(uint8_t top, uint8_t bottom) {
    uint8_t rows = framebuffer_get_rows();
    if (bottom > rows)
        bottom = rows;
    if (top >= bottom)
        return;
    region_top    = top;
    region_bottom = bottom;
    if (console_row < top || console_row >= bottom) {
        console_row = top;
        console_col = 0;
        console_sync_cursor();
    }
}

void console_get_region(uint8_t *top, uint8_t *bottom) {
    *top    = region_top;
    *bottom = region_bottom;
}

void console_clear(void) {
    uint8_t cols = framebuffer_get_cols();
    for (uint8_t r = region_top; r < region_bottom; r++)
        for (uint8_t c = 0; c < cols; c++)
            framebuffer_write(r, c, ' ', console_fg, console_bg);
    console_row = region_top;
    console_col = 0;
    console_sync_cursor();
}

void console_set_color(uint8_t fg, uint8_t bg) {
    console_fg = fg & 0xF;
    console_bg = bg & 0xF;
}

void console_get_color(uint8_t *fg, uint8_t *bg) {
    *fg = console_fg;
    *bg = console_bg;
}

static void console_putchar_nosync(char c) {
    uint8_t cols = framebuffer_get_cols();
    switch (c) {
        case '\n':
            console_newline();
            break;
        case '\r':
            console_col = 0;
            break;
        case '\b':
            if (console_col > 0) {
                console_col--;
            } else if (console_row > region_top) {
                console_row--;
                console_col = cols - 1;
            }
            framebuffer_write(console_row, console_col, ' ', console_fg, console_bg);
            break;
        case '\t': {
            uint8_t spaces = TAB_WIDTH - (console_col % TAB_WIDTH);
            for (uint8_t i = 0; i < spaces; i++)
                console_putchar_nosync(' ');
            break;
        }
        default:
            framebuffer_write(console_row, console_col, c, console_fg, console_bg);
            if (++console_col >= cols)
                console_newline();
            break;
    }
}

void console_putchar(char c) {
    console_putchar_nosync(c);
    if (c != '\b')
        serial_putc(c);
    console_sync_cursor();
}

void console_write(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        console_putchar_nosync(s[i]);
        if (s[i] != '\b')
            serial_putc(s[i]);
    }
    console_sync_cursor();
}

void console_puts(const char *s) {
    size_t len = 0;
    while (s[len] != '\0')
        len++;
    console_write(s, len);
}

void console_write_color(const char *s, size_t len, uint8_t fg, uint8_t bg) {
    uint8_t old_fg = console_fg, old_bg = console_bg;
    console_set_color(fg, bg);
    console_write(s, len);
    console_set_color(old_fg, old_bg);
}

void console_set_cursor(uint8_t row, uint8_t col) {
    if (row < region_top)
        row = region_top;
    if (row >= region_bottom)
        row = region_bottom - 1;
    if (col >= framebuffer_get_cols())
        col = framebuffer_get_cols() - 1;
    console_row = row;
    console_col = col;
    console_sync_cursor();
}

void console_get_cursor(uint8_t *row, uint8_t *col) {
    *row = console_row;
    *col = console_col;
}

int kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_puts(buf);
    return len;
}
