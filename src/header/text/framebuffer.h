#ifndef _FRAMEBUFFER_H
#define _FRAMEBUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FRAMEBUFFER_MEMORY_OFFSET ((uint8_t*) 0xC00B8000) // VGA text memory 0xB8000, seen through the higher half
#define CURSOR_PORT_CMD    0x03D4
#define CURSOR_PORT_DATA   0x03D5

// VGA text mode resolution (mode 3h)
#define FRAMEBUFFER_WIDTH  80
#define FRAMEBUFFER_HEIGHT 25

// Upper bound of the character grid in graphics mode (1024x768 with an 8x16 font)
#define FRAMEBUFFER_MAX_COLS 128
#define FRAMEBUFFER_MAX_ROWS 48

/**
 * Terminal framebuffer
 * Resolution: 80x25 in VGA text mode, 128x48 cells in BGA graphics mode
 * Starting at FRAMEBUFFER_MEMORY_OFFSET (text mode),
 * - Even number memory: Character, 8-bit
 * - Odd number memory:  Character color lower 4-bit, Background color upper 4-bit
 *
 * In graphics mode the same character-cell interface is rendered with an 8x16
 * bitmap font into the linear framebuffer, so every caller keeps working unchanged.
*/

/**
 * Set framebuffer character and color with corresponding parameter values.
 * More details: https://en.wikipedia.org/wiki/BIOS_color_attributes
 * Writes outside the visible grid are ignored.
 *
 * @param row Vertical location (index start 0)
 * @param col Horizontal location (index start 0)
 * @param c   Character
 * @param fg  Foreground / Character color
 * @param bg  Background color
 */
void framebuffer_write(uint8_t row, uint8_t col, char c, uint8_t fg, uint8_t bg);

/**
 * Set cursor to specified location. Row and column starts from 0
 *
 * @param r row
 * @param c column
*/
void framebuffer_set_cursor(uint8_t r, uint8_t c);

/**
 * Set all cell in framebuffer character to 0x00 (empty character)
 * and color to 0x07 (gray character & black background)
 * Extra note: It's allowed to use different color palette for this
 *
 */
void framebuffer_clear(void);

/* -- Extensions (display bonus, console support) -- */

/**
 * Choose the display backend. Must be called before any other framebuffer function
 * when graphics mode is wanted; without it the driver behaves like the plain text driver.
 *
 * @param prefer_graphics Try the BGA 1024x768x32 mode first, fall back to text mode
 * @return true if graphics mode is active
 */
bool framebuffer_init(bool prefer_graphics);

// Whether the backend is the BGA linear framebuffer
bool framebuffer_is_graphics(void);

// Current character grid size
uint8_t framebuffer_get_rows(void);
uint8_t framebuffer_get_cols(void);

// Current cursor position
void framebuffer_get_cursor(uint8_t *r, uint8_t *c);

/**
 * Read back one cell
 *
 * @param c  Output character (may be NULL)
 * @param fg Output foreground color (may be NULL)
 * @param bg Output background color (may be NULL)
 */
void framebuffer_read(uint8_t row, uint8_t col, char *c, uint8_t *fg, uint8_t *bg);

/**
 * Scroll rows [top, bottom) up by one line. The last row of the region is
 * filled with spaces using `bg` as background color.
 */
void framebuffer_scroll_region(uint8_t top, uint8_t bottom, uint8_t bg);

// Show or hide the text cursor
void framebuffer_set_cursor_visible(bool visible);

/**
 * Software cursor blinking for graphics mode (text mode blinks in hardware).
 * Call periodically from idle loops with the current time in milliseconds.
 */
void framebuffer_cursor_tick(uint32_t now_ms);

/**
 * Mouse pointer overlay. Coordinates are in pixels: 1024x768 in graphics mode,
 * cols*8 x rows*16 (640x400) in text mode where the pointer is an inverted cell.
 */
void framebuffer_get_pointer_space(uint16_t *width, uint16_t *height);
void framebuffer_pointer_move(int x, int y);
void framebuffer_pointer_set_visible(bool visible);

#endif
