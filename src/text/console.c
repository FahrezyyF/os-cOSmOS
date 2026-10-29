#include <stdarg.h>
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/driver/serial.h"
#include "header/memory/kheap.h"
#include "header/stdlib/format.h"

#define TAB_WIDTH 4
#define MAKE_CELL(c, fg, bg) ((uint16_t) ((uint8_t) (c) | ((uint16_t) ((fg) & 0xF) << 8) | ((uint16_t) ((bg) & 0xF) << 12)))

static uint8_t console_row    = 0;
static uint8_t console_col    = 0;
static uint8_t console_fg     = COLOR_LIGHT_GRAY;
static uint8_t console_bg     = COLOR_BLACK;
static uint8_t region_top     = 0;
static uint8_t region_bottom  = FRAMEBUFFER_HEIGHT;

/**
 * Scrollback: a ring of the lines that scrolled off the top of the region (kernel heap).
 * While the view is scrolled back the live region is kept in `saved_live` and restored on
 * the next output, so programs never see the difference.
 */
static uint16_t *history          = NULL;
static uint32_t  history_capacity = 0;
static uint32_t  history_count    = 0;
static uint32_t  history_head     = 0;  // oldest line
static uint8_t   history_cols     = 0;
static uint16_t *saved_live       = NULL;
static uint32_t  view_offset      = 0;

static void console_sync_cursor(void) {
    framebuffer_set_cursor(console_row, console_col);
}

static void history_push_row(uint8_t row) {
    if (history_capacity == 0)
        return;
    uint32_t slot;
    if (history_count < history_capacity) {
        slot = (history_head + history_count) % history_capacity;
        history_count++;
    } else {
        slot = history_head; // overwrite the oldest line
        history_head = (history_head + 1) % history_capacity;
    }
    uint16_t *line = &history[slot * history_cols];
    for (uint8_t c = 0; c < history_cols; c++) {
        char ch;
        uint8_t fg, bg;
        framebuffer_read(row, c, &ch, &fg, &bg);
        line[c] = MAKE_CELL(ch, fg, bg);
    }
}

static void console_newline(void) {
    console_col = 0;
    if (console_row + 1 < region_bottom) {
        console_row++;
    } else {
        history_push_row(region_top);
        framebuffer_scroll_region(region_top, region_bottom, console_bg);
        console_row = region_bottom - 1;
    }
}

static void draw_cells(uint8_t row, const uint16_t *cells) {
    for (uint8_t c = 0; c < history_cols; c++)
        framebuffer_write(row, c, (char) (cells[c] & 0xFF), (cells[c] >> 8) & 0xF, (cells[c] >> 12) & 0xF);
}

static void redraw_view(void) {
    uint32_t rows = region_bottom - region_top;
    for (uint32_t r = 0; r < rows; r++) {
        // virtual line index: history lines first, then the saved live region
        uint32_t v = history_count - view_offset + r;
        if (v < history_count)
            draw_cells((uint8_t) (region_top + r), &history[((history_head + v) % history_capacity) * history_cols]);
        else
            draw_cells((uint8_t) (region_top + r), &saved_live[(v - history_count) * history_cols]);
    }
}

void console_scroll_live(void) {
    if (view_offset == 0)
        return;
    view_offset = 0;
    redraw_view();
    framebuffer_set_cursor_visible(true);
    console_sync_cursor();
}

uint32_t console_scroll_view(int32_t delta) {
    if (history_count == 0 || delta == 0) {
        if (delta == 0)
            console_scroll_live();
        return view_offset;
    }
    if (view_offset == 0 && delta < 0) {
        // leaving the live view: remember what the region looks like now
        uint32_t rows = region_bottom - region_top;
        for (uint32_t r = 0; r < rows; r++) {
            for (uint8_t c = 0; c < history_cols; c++) {
                char ch;
                uint8_t fg, bg;
                framebuffer_read((uint8_t) (region_top + r), c, &ch, &fg, &bg);
                saved_live[r * history_cols + c] = MAKE_CELL(ch, fg, bg);
            }
        }
        framebuffer_set_cursor_visible(false);
    }
    int64_t target = (int64_t) view_offset - delta;
    if (target < 0)
        target = 0;
    if (target > (int64_t) history_count)
        target = history_count;
    if (target == 0) {
        console_scroll_live();
        return 0;
    }
    view_offset = (uint32_t) target;
    redraw_view();
    return view_offset;
}

uint32_t console_view_offset(void) {
    return view_offset;
}

uint32_t console_history_lines(void) {
    return history_count;
}

bool console_enable_scrollback(uint32_t lines) {
    uint8_t cols = framebuffer_get_cols();
    uint8_t rows = framebuffer_get_rows();
    if (lines == 0 || history != NULL)
        return history != NULL;
    history    = (uint16_t*) kmalloc((size_t) lines * cols * sizeof(uint16_t));
    saved_live = (uint16_t*) kmalloc((size_t) rows * cols * sizeof(uint16_t));
    if (history == NULL || saved_live == NULL) {
        kfree(history);
        kfree(saved_live);
        history = saved_live = NULL;
        return false;
    }
    history_capacity = lines;
    history_cols     = cols;
    history_count    = 0;
    history_head     = 0;
    view_offset      = 0;
    return true;
}

void console_init(void) {
    view_offset   = 0; // (the panic screen starts over without restoring anything)
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
    console_scroll_live();
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
    console_scroll_live();
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
    console_scroll_live();
    console_putchar_nosync(c);
    if (c != '\b')
        serial_putc(c);
    console_sync_cursor();
}

void console_write(const char *s, size_t len) {
    console_scroll_live();
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
    console_scroll_live();
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
