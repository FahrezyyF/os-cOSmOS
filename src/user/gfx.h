#ifndef _USER_GFX_H
#define _USER_GFX_H

#include "user/ulib.h"

/**
 * Tiny drawing library for window programs (bonus: window compositor). A Canvas is the
 * program's own pixel buffer (heap memory, 0x00RRGGBB); canvas_present() hands it to the
 * kernel compositor, which copies it into the window on screen.
 */

#define GFX_RGB(r, g, b) ((((uint32_t) (r) & 0xFF) << 16) | (((uint32_t) (g) & 0xFF) << 8) | ((uint32_t) (b) & 0xFF))

struct Canvas {
    uint32_t *px;
    int32_t   w, h;
    int32_t   window;   // compositor window id
};

// Allocate the buffer and open a window (x, y < 0: let the compositor place it)
bool canvas_open(struct Canvas *c, int32_t w, int32_t h, const char *title, int32_t x, int32_t y);
void canvas_present(const struct Canvas *c);
// Next event (block = sleep until one arrives); type SYSCALL_WEVENT_NONE when none
void canvas_event(const struct Canvas *c, struct SyscallWindowEvent *e, bool block);
void canvas_title(const struct Canvas *c, const char *title);
void canvas_close(struct Canvas *c);
bool gfx_available(void); // graphics mode with the compositor

void gfx_clear(struct Canvas *c, uint32_t rgb);
void gfx_pixel(struct Canvas *c, int32_t x, int32_t y, uint32_t rgb);
void gfx_rect(struct Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb);
void gfx_line(struct Canvas *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb);
void gfx_thick_line(struct Canvas *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t r, uint32_t rgb);
void gfx_circle(struct Canvas *c, int32_t cx, int32_t cy, int32_t r, uint32_t rgb);       // filled
void gfx_ring(struct Canvas *c, int32_t cx, int32_t cy, int32_t r, uint32_t rgb);         // outline
void gfx_text(struct Canvas *c, int32_t x, int32_t y, const char *s, uint32_t rgb, int32_t scale);

#endif
