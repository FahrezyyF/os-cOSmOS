#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/text/framebuffer.h"
#include "header/stdlib/string.h"
#include "header/cpu/portio.h"

void framebuffer_set_cursor(uint8_t r, uint8_t c) {
    uint16_t pos = r * 80 + c;
    out(CURSOR_PORT_CMD, 0x0F);
    out(CURSOR_PORT_DATA, (uint8_t) (pos & 0xFF));
    out(CURSOR_PORT_CMD, 0x0E);
    out(CURSOR_PORT_DATA, (uint8_t) ((pos >> 8) & 0xFF));
}

void framebuffer_write(uint8_t row, uint8_t col, char c, uint8_t fg, uint8_t bg) {
    uint16_t offset = (row * 80 + col) * 2;
    FRAMEBUFFER_MEMORY_OFFSET[offset] = c;
    FRAMEBUFFER_MEMORY_OFFSET[offset + 1] = (bg << 4) | (fg & 0xF);
}

void framebuffer_clear(void) {
    for(int r = 0 ; r < 25 ; r++){
        for(int c = 0 ; c < 80 ; c++){
            framebuffer_write(r, c, 0x00, 0x7, 0x0);
        }
    }
}
