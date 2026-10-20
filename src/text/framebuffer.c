#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/text/framebuffer.h"
#include "header/text/font.h"
#include "header/graphics/graphics.h"
#include "header/stdlib/string.h"
#include "header/cpu/portio.h"

#define DEFAULT_ATTR         0x07
#define MAKE_CELL(c, attr)   ((uint16_t) ((uint8_t) (c) | ((uint16_t) (attr) << 8)))
#define CURSOR_BLINK_MS      500
#define POINTER_WIDTH        11
#define POINTER_HEIGHT       18

static bool     graphics_mode   = false;
static uint8_t  grid_rows       = FRAMEBUFFER_HEIGHT;
static uint8_t  grid_cols       = FRAMEBUFFER_WIDTH;
static uint8_t  cursor_row      = 0;
static uint8_t  cursor_col      = 0;
static bool     cursor_visible  = true;
static bool     cursor_phase_on = true;
static uint32_t cursor_last_toggle_ms = 0;

// Graphics mode keeps the character grid in RAM so cells can be redrawn / read back
static uint16_t shadow_cells[FRAMEBUFFER_MAX_ROWS][FRAMEBUFFER_MAX_COLS];

// Mouse pointer overlay state
static bool     pointer_visible = false;
static bool     pointer_drawn   = false;
static int      pointer_x       = 0;
static int      pointer_y       = 0;
static uint32_t pointer_saved_pixels[POINTER_HEIGHT][POINTER_WIDTH];
static uint8_t  pointer_saved_attr = 0;

// Classic arrow: 'X' = black outline, '.' = white fill, ' ' = transparent
static const char *pointer_sprite[POINTER_HEIGHT] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X........X ",
    "X.....XXXXX",
    "X..X..X    ",
    "X.X X..X   ",
    "XX  X..X   ",
    "X    X..X  ",
    "     X..X  ",
    "      XX   ",
    "           ",
};

/* -- VGA text mode backend -- */

static volatile uint16_t *text_vram(void) {
    return (volatile uint16_t*) FRAMEBUFFER_MEMORY_OFFSET;
}

static void text_set_hw_cursor(uint8_t r, uint8_t c) {
    uint16_t pos = (uint16_t) (r * FRAMEBUFFER_WIDTH + c);
    out(CURSOR_PORT_CMD, 0x0F);
    out(CURSOR_PORT_DATA, (uint8_t) (pos & 0xFF));
    out(CURSOR_PORT_CMD, 0x0E);
    out(CURSOR_PORT_DATA, (uint8_t) ((pos >> 8) & 0xFF));
}

static void text_enable_hw_cursor(bool enable) {
    out(CURSOR_PORT_CMD, 0x0A);
    if (enable) {
        out(CURSOR_PORT_DATA, (uint8_t) ((in(CURSOR_PORT_DATA) & 0xC0) | 13)); // scanline start
        out(CURSOR_PORT_CMD, 0x0B);
        out(CURSOR_PORT_DATA, (uint8_t) ((in(CURSOR_PORT_DATA) & 0xE0) | 14)); // scanline end
    } else {
        out(CURSOR_PORT_DATA, 0x20); // bit 5 disables the cursor
    }
}

/* -- Graphics backend helpers -- */

static void graphics_draw_cell(uint8_t row, uint8_t col) {
    uint16_t cell = shadow_cells[row][col];
    uint8_t  attr = (uint8_t) (cell >> 8);
    uint32_t fg   = graphics_vga_palette[attr & 0xF];
    uint32_t bg   = graphics_vga_palette[(attr >> 4) & 0xF];
    int      x    = col * FONT_WIDTH;
    int      y    = row * FONT_HEIGHT;
    graphics_draw_glyph(x, y, (uint8_t) (cell & 0xFF), fg, bg);
    if (row == cursor_row && col == cursor_col && cursor_visible && cursor_phase_on)
        graphics_fill_rect(x, y + FONT_HEIGHT - 2, FONT_WIDTH, 2, fg); // underline cursor
}

/* -- Pointer overlay -- */

static void pointer_cell(uint8_t *row, uint8_t *col) {
    *row = (uint8_t) (pointer_y / FONT_HEIGHT);
    *col = (uint8_t) (pointer_x / FONT_WIDTH);
}

static void pointer_draw(void) {
    if (!pointer_visible || pointer_drawn)
        return;
    if (graphics_mode) {
        for (int r = 0; r < POINTER_HEIGHT; r++) {
            for (int c = 0; c < POINTER_WIDTH; c++) {
                pointer_saved_pixels[r][c] = graphics_get_pixel(pointer_x + c, pointer_y + r);
                char p = pointer_sprite[r][c];
                if (p == 'X')
                    graphics_put_pixel(pointer_x + c, pointer_y + r, RGB(0, 0, 0));
                else if (p == '.')
                    graphics_put_pixel(pointer_x + c, pointer_y + r, RGB(255, 255, 255));
            }
        }
    } else {
        uint8_t row, col;
        pointer_cell(&row, &col);
        volatile uint16_t *cell = &text_vram()[row * FRAMEBUFFER_WIDTH + col];
        pointer_saved_attr = (uint8_t) (*cell >> 8);
        *cell = MAKE_CELL(*cell & 0xFF, pointer_saved_attr ^ 0x77);
    }
    pointer_drawn = true;
}

static void pointer_erase(void) {
    if (!pointer_drawn)
        return;
    if (graphics_mode) {
        for (int r = 0; r < POINTER_HEIGHT; r++)
            for (int c = 0; c < POINTER_WIDTH; c++)
                graphics_put_pixel(pointer_x + c, pointer_y + r, pointer_saved_pixels[r][c]);
    } else {
        uint8_t row, col;
        pointer_cell(&row, &col);
        volatile uint16_t *cell = &text_vram()[row * FRAMEBUFFER_WIDTH + col];
        *cell = MAKE_CELL(*cell & 0xFF, pointer_saved_attr);
    }
    pointer_drawn = false;
}

// Erase the pointer when it overlaps the cell about to be redrawn; returns whether to redraw it
static bool pointer_begin_cell_update(uint8_t row, uint8_t col) {
    if (!pointer_drawn)
        return false;
    bool overlap;
    if (graphics_mode) {
        int x = col * FONT_WIDTH, y = row * FONT_HEIGHT;
        overlap = pointer_x < x + FONT_WIDTH && pointer_x + POINTER_WIDTH > x &&
                  pointer_y < y + FONT_HEIGHT && pointer_y + POINTER_HEIGHT > y;
    } else {
        uint8_t prow, pcol;
        pointer_cell(&prow, &pcol);
        overlap = prow == row && pcol == col;
    }
    if (overlap)
        pointer_erase();
    return overlap;
}

/* -- Book interface -- */

void framebuffer_write(uint8_t row, uint8_t col, char c, uint8_t fg, uint8_t bg) {
    if (row >= grid_rows || col >= grid_cols)
        return;
    uint8_t attr        = (uint8_t) (((bg & 0xF) << 4) | (fg & 0xF));
    bool    redraw_ptr  = pointer_begin_cell_update(row, col);
    if (graphics_mode) {
        shadow_cells[row][col] = MAKE_CELL(c, attr);
        graphics_draw_cell(row, col);
    } else {
        uint16_t offset = (uint16_t) ((row * FRAMEBUFFER_WIDTH + col) * 2);
        FRAMEBUFFER_MEMORY_OFFSET[offset]     = (uint8_t) c;
        FRAMEBUFFER_MEMORY_OFFSET[offset + 1] = attr;
    }
    if (redraw_ptr)
        pointer_draw();
}

void framebuffer_set_cursor(uint8_t r, uint8_t c) {
    if (r >= grid_rows) r = grid_rows - 1;
    if (c >= grid_cols) c = grid_cols - 1;
    if (!graphics_mode) {
        cursor_row = r;
        cursor_col = c;
        text_set_hw_cursor(r, c);
        return;
    }
    uint8_t old_row = cursor_row, old_col = cursor_col;
    cursor_row      = r;
    cursor_col      = c;
    cursor_phase_on = true; // keep the cursor solid while the user is typing
    bool redraw_old = pointer_begin_cell_update(old_row, old_col);
    graphics_draw_cell(old_row, old_col);
    if (redraw_old)
        pointer_draw();
    bool redraw_new = pointer_begin_cell_update(r, c);
    graphics_draw_cell(r, c);
    if (redraw_new)
        pointer_draw();
}

void framebuffer_clear(void) {
    bool had_pointer = pointer_drawn;
    pointer_erase();
    if (graphics_mode) {
        for (uint8_t r = 0; r < FRAMEBUFFER_MAX_ROWS; r++)
            for (uint8_t c = 0; c < FRAMEBUFFER_MAX_COLS; c++)
                shadow_cells[r][c] = MAKE_CELL(0, DEFAULT_ATTR);
        graphics_clear(graphics_vga_palette[0]);
        graphics_draw_cell(cursor_row, cursor_col);
    } else {
        for (uint8_t r = 0; r < FRAMEBUFFER_HEIGHT; r++)
            for (uint8_t c = 0; c < FRAMEBUFFER_WIDTH; c++)
                text_vram()[r * FRAMEBUFFER_WIDTH + c] = MAKE_CELL(0, DEFAULT_ATTR);
    }
    if (had_pointer)
        pointer_draw();
}

/* -- Extensions -- */

bool framebuffer_init(bool prefer_graphics) {
    if (prefer_graphics && graphics_init(GRAPHICS_DEFAULT_WIDTH, GRAPHICS_DEFAULT_HEIGHT)) {
        graphics_mode = true;
        grid_cols     = (uint8_t) (graphics_get_width() / FONT_WIDTH);
        grid_rows     = (uint8_t) (graphics_get_height() / FONT_HEIGHT);
        if (grid_cols > FRAMEBUFFER_MAX_COLS) grid_cols = FRAMEBUFFER_MAX_COLS;
        if (grid_rows > FRAMEBUFFER_MAX_ROWS) grid_rows = FRAMEBUFFER_MAX_ROWS;
    } else {
        graphics_mode = false;
        grid_cols     = FRAMEBUFFER_WIDTH;
        grid_rows     = FRAMEBUFFER_HEIGHT;
        text_enable_hw_cursor(true);
    }
    cursor_row = cursor_col = 0;
    framebuffer_clear();
    framebuffer_set_cursor(0, 0);
    return graphics_mode;
}

bool framebuffer_is_graphics(void) {
    return graphics_mode;
}

uint8_t framebuffer_get_rows(void) {
    return grid_rows;
}

uint8_t framebuffer_get_cols(void) {
    return grid_cols;
}

void framebuffer_get_cursor(uint8_t *r, uint8_t *c) {
    *r = cursor_row;
    *c = cursor_col;
}

void framebuffer_read(uint8_t row, uint8_t col, char *c, uint8_t *fg, uint8_t *bg) {
    uint16_t cell = 0;
    if (row < grid_rows && col < grid_cols) {
        if (graphics_mode) {
            cell = shadow_cells[row][col];
        } else {
            cell = text_vram()[row * FRAMEBUFFER_WIDTH + col];
            uint8_t prow, pcol;
            pointer_cell(&prow, &pcol);
            if (pointer_drawn && prow == row && pcol == col)
                cell = MAKE_CELL(cell & 0xFF, pointer_saved_attr);
        }
    }
    if (c)  *c  = (char) (cell & 0xFF);
    if (fg) *fg = (uint8_t) ((cell >> 8) & 0xF);
    if (bg) *bg = (uint8_t) ((cell >> 12) & 0xF);
}

void framebuffer_scroll_region(uint8_t top, uint8_t bottom, uint8_t bg) {
    if (bottom > grid_rows)
        bottom = grid_rows;
    if (top >= bottom)
        return;
    uint8_t blank_attr  = (uint8_t) (((bg & 0xF) << 4) | 0x7);
    bool    had_pointer = pointer_drawn;
    pointer_erase();

    if (graphics_mode) {
        // Hide the cursor underline so it does not get dragged up with the pixels
        bool phase = cursor_phase_on;
        cursor_phase_on = false;
        if (cursor_row >= top && cursor_row < bottom)
            graphics_draw_cell(cursor_row, cursor_col);
        cursor_phase_on = phase;

        for (uint8_t r = top; r + 1 < bottom; r++)
            memcpy(shadow_cells[r], shadow_cells[r + 1], sizeof(shadow_cells[r]));
        for (uint8_t c = 0; c < FRAMEBUFFER_MAX_COLS; c++)
            shadow_cells[bottom - 1][c] = MAKE_CELL(' ', blank_attr);
        graphics_scroll_up(top * FONT_HEIGHT, bottom * FONT_HEIGHT, FONT_HEIGHT, graphics_vga_palette[bg & 0xF]);
        graphics_draw_cell(cursor_row, cursor_col);
    } else {
        volatile uint16_t *vram = text_vram();
        for (uint8_t r = top; r + 1 < bottom; r++)
            for (uint8_t c = 0; c < FRAMEBUFFER_WIDTH; c++)
                vram[r * FRAMEBUFFER_WIDTH + c] = vram[(r + 1) * FRAMEBUFFER_WIDTH + c];
        for (uint8_t c = 0; c < FRAMEBUFFER_WIDTH; c++)
            vram[(bottom - 1) * FRAMEBUFFER_WIDTH + c] = MAKE_CELL(' ', blank_attr);
    }

    if (had_pointer)
        pointer_draw();
}

void framebuffer_set_cursor_visible(bool visible) {
    cursor_visible = visible;
    if (graphics_mode) {
        bool redraw = pointer_begin_cell_update(cursor_row, cursor_col);
        graphics_draw_cell(cursor_row, cursor_col);
        if (redraw)
            pointer_draw();
    } else {
        text_enable_hw_cursor(visible);
    }
}

void framebuffer_cursor_tick(uint32_t now_ms) {
    if (!graphics_mode || !cursor_visible)
        return;
    if (now_ms - cursor_last_toggle_ms < CURSOR_BLINK_MS)
        return;
    cursor_last_toggle_ms = now_ms;
    cursor_phase_on       = !cursor_phase_on;
    bool redraw = pointer_begin_cell_update(cursor_row, cursor_col);
    graphics_draw_cell(cursor_row, cursor_col);
    if (redraw)
        pointer_draw();
}

void framebuffer_get_pointer_space(uint16_t *width, uint16_t *height) {
    *width  = (uint16_t) (grid_cols * FONT_WIDTH);
    *height = (uint16_t) (grid_rows * FONT_HEIGHT);
}

void framebuffer_pointer_move(int x, int y) {
    uint16_t w, h;
    framebuffer_get_pointer_space(&w, &h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    if (x == pointer_x && y == pointer_y)
        return;
    bool had_pointer = pointer_drawn;
    pointer_erase();
    pointer_x = x;
    pointer_y = y;
    if (had_pointer || pointer_visible)
        pointer_draw();
}

void framebuffer_pointer_set_visible(bool visible) {
    if (!visible)
        pointer_erase();
    pointer_visible = visible;
    if (visible)
        pointer_draw();
}
