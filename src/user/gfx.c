#include "user/gfx.h"
#include "header/text/font.h"

bool gfx_available(void) {
    struct SyscallWindowInfo info;
    memset(&info, 0, sizeof(info));
    syscall(SYS_WIN_INFO, (uint32_t) &info, 0, 0);
    return info.available;
}

bool canvas_open(struct Canvas *c, int32_t w, int32_t h, const char *title, int32_t x, int32_t y) {
    c->w = w;
    c->h = h;
    c->px = malloc((uint32_t) (w * h) * sizeof(uint32_t));
    c->window = -1;
    if (c->px == NULL)
        return false;
    struct SyscallWindow req;
    memset(&req, 0, sizeof(req));
    req.x = x;
    req.y = y;
    req.width = (uint32_t) w;
    req.height = (uint32_t) h;
    strncpy(req.title, title, sizeof(req.title) - 1);
    syscall(SYS_WIN_CREATE, (uint32_t) &req, (uint32_t) &c->window, 0);
    if (c->window < 0) {
        free(c->px);
        c->px = NULL;
        return false;
    }
    return true;
}

void canvas_present(const struct Canvas *c) {
    syscall(SYS_WIN_PRESENT, (uint32_t) c->window, (uint32_t) c->px, 0);
}

void canvas_event(const struct Canvas *c, struct SyscallWindowEvent *e, bool block) {
    memset(e, 0, sizeof(*e));
    syscall(SYS_WIN_EVENT, (uint32_t) c->window, (uint32_t) e, block ? 1 : 0);
}

void canvas_title(const struct Canvas *c, const char *title) {
    syscall(SYS_WIN_SET_TITLE, (uint32_t) c->window, (uint32_t) title, 0);
}

void canvas_close(struct Canvas *c) {
    if (c->window >= 0)
        syscall(SYS_WIN_DESTROY, (uint32_t) c->window, 0, 0);
    free(c->px);
    c->px = NULL;
    c->window = -1;
}

void gfx_clear(struct Canvas *c, uint32_t rgb) {
    for (int32_t i = 0; i < c->w * c->h; i++)
        c->px[i] = rgb;
}

void gfx_pixel(struct Canvas *c, int32_t x, int32_t y, uint32_t rgb) {
    if (x >= 0 && y >= 0 && x < c->w && y < c->h)
        c->px[y * c->w + x] = rgb;
}

void gfx_rect(struct Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    int32_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    for (int32_t yy = y0; yy < y1; yy++)
        for (int32_t xx = x0; xx < x1; xx++)
            c->px[yy * c->w + xx] = rgb;
}

void gfx_line(struct Canvas *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb) {
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        gfx_pixel(c, x0, y0, rgb);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_circle(struct Canvas *c, int32_t cx, int32_t cy, int32_t r, uint32_t rgb) {
    for (int32_t y = -r; y <= r; y++)
        for (int32_t x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                gfx_pixel(c, cx + x, cy + y, rgb);
}

void gfx_ring(struct Canvas *c, int32_t cx, int32_t cy, int32_t r, uint32_t rgb) {
    int32_t x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(c, cx + x, cy + y, rgb); gfx_pixel(c, cx - x, cy + y, rgb);
        gfx_pixel(c, cx + x, cy - y, rgb); gfx_pixel(c, cx - x, cy - y, rgb);
        gfx_pixel(c, cx + y, cy + x, rgb); gfx_pixel(c, cx - y, cy + x, rgb);
        gfx_pixel(c, cx + y, cy - x, rgb); gfx_pixel(c, cx - y, cy - x, rgb);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_thick_line(struct Canvas *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t r, uint32_t rgb) {
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0)
        steps = 1;
    for (int32_t i = 0; i <= steps; i++)
        gfx_circle(c, x0 + dx * i / steps, y0 + dy * i / steps, r, rgb);
}

void gfx_text(struct Canvas *c, int32_t x, int32_t y, const char *s, uint32_t rgb, int32_t scale) {
    for (; *s != '\0'; s++, x += FONT_WIDTH * scale) {
        const uint8_t *glyph = font8x16[(uint8_t) *s];
        for (int32_t row = 0; row < FONT_HEIGHT; row++)
            for (int32_t col = 0; col < FONT_WIDTH; col++)
                if (glyph[row] & (0x80 >> col))
                    gfx_rect(c, x + col * scale, y + row * scale, scale, scale, rgb);
    }
}
