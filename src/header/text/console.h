#ifndef _CONSOLE_H
#define _CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// VGA text attribute colors
#define COLOR_BLACK         0x0
#define COLOR_BLUE          0x1
#define COLOR_GREEN         0x2
#define COLOR_CYAN          0x3
#define COLOR_RED           0x4
#define COLOR_MAGENTA       0x5
#define COLOR_BROWN         0x6
#define COLOR_LIGHT_GRAY    0x7
#define COLOR_DARK_GRAY     0x8
#define COLOR_LIGHT_BLUE    0x9
#define COLOR_LIGHT_GREEN   0xA
#define COLOR_LIGHT_CYAN    0xB
#define COLOR_LIGHT_RED     0xC
#define COLOR_LIGHT_MAGENTA 0xD
#define COLOR_YELLOW        0xE
#define COLOR_WHITE         0xF

/**
 * Teletype-style text console built on the framebuffer driver.
 * Handles cursor movement, line wrapping, scrolling inside a region (so a status
 * bar can stay pinned), '\n', '\r', '\b' (move back and erase) and '\t'.
 * Everything printed is mirrored to the COM1 serial log.
 */

// Use the whole screen as the scrolling region and clear it
void console_init(void);

/**
 * Restrict scrolling/printing to rows [top, bottom). Rows outside the region
 * (e.g. a status bar) are never touched by the console.
 */
void console_set_region(uint8_t top, uint8_t bottom);
void console_get_region(uint8_t *top, uint8_t *bottom);

// Clear the console region and move the cursor to its top-left
void console_clear(void);

void console_set_color(uint8_t fg, uint8_t bg);
void console_get_color(uint8_t *fg, uint8_t *bg);

void console_putchar(char c);
void console_write(const char *s, size_t len);
void console_puts(const char *s);

// Print with a temporary color, restoring the previous one afterwards
void console_write_color(const char *s, size_t len, uint8_t fg, uint8_t bg);

void console_set_cursor(uint8_t row, uint8_t col);
void console_get_cursor(uint8_t *row, uint8_t *col);

// printf-style output to the console (and serial log)
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
