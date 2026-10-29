#ifndef _GRAPHICS_H
#define _GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Bochs Graphics Adapter (BGA / "VBE DISPI") driver.
 * QEMU's default `-vga std` card (PCI 1234:1111), Bochs, and VirtualBox implement it.
 * Programming it through two I/O ports switches the card into a linear 32-bit
 * framebuffer mode where every pixel is addressable (bonus: display output & graphics driver).
 */
#define BGA_INDEX_PORT          0x01CE
#define BGA_DATA_PORT           0x01CF

#define BGA_REG_ID              0
#define BGA_REG_XRES            1
#define BGA_REG_YRES            2
#define BGA_REG_BPP             3
#define BGA_REG_ENABLE          4
#define BGA_REG_VIRT_WIDTH      6
#define BGA_REG_VIRT_HEIGHT     7
#define BGA_REG_X_OFFSET        8
#define BGA_REG_Y_OFFSET        9

#define BGA_ID_MIN              0xB0C2 // first version with 32 bpp + linear framebuffer
#define BGA_ID_MAX              0xB0C5
#define BGA_DISABLED            0x00
#define BGA_ENABLED             0x01
#define BGA_LFB_ENABLED         0x40

#define BGA_PCI_VENDOR          0x1234
#define BGA_PCI_DEVICE          0x1111
#define BGA_VBOX_PCI_VENDOR     0x80EE
#define BGA_VBOX_PCI_DEVICE     0xBEEF
#define BGA_DEFAULT_LFB_ADDRESS 0xFD000000u

#define GRAPHICS_DEFAULT_WIDTH  1024
#define GRAPHICS_DEFAULT_HEIGHT 768
#define GRAPHICS_BPP            32

// 0x00RRGGBB pixel helper
#define RGB(r, g, b) ((((uint32_t) (r) & 0xFF) << 16) | (((uint32_t) (g) & 0xFF) << 8) | ((uint32_t) (b) & 0xFF))

// The 16 classic VGA text colors as RGB, indexed by the 4-bit text attribute
extern const uint32_t graphics_vga_palette[16];

/**
 * Detect the BGA, locate its linear framebuffer via PCI BAR0 and switch to
 * width x height x 32 bpp.
 *
 * @return true when graphics mode is active, false when the adapter is missing
 *         (the caller should stay in VGA text mode)
 */
bool graphics_init(uint16_t width, uint16_t height);

bool     graphics_is_active(void);
uint16_t graphics_get_width(void);
uint16_t graphics_get_height(void);
uint16_t graphics_get_bga_version(void);
uint32_t graphics_get_lfb_address(void); // physical address (PCI BAR0); mapped at KERNEL_LFB_BASE

// Pixel primitives, all coordinates are clipped to the screen
void     graphics_put_pixel(int x, int y, uint32_t rgb);
uint32_t graphics_get_pixel(int x, int y);
void     graphics_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void     graphics_clear(uint32_t rgb);

/**
 * Draw one 8x16 glyph with an opaque background (text console cell)
 */
void graphics_draw_glyph(int x, int y, uint8_t ch, uint32_t fg, uint32_t bg);

/**
 * Draw one glyph scaled by an integer factor, background pixels untouched
 */
void graphics_draw_glyph_scaled(int x, int y, uint8_t ch, uint32_t fg, int scale);

// Draw a null-terminated string with graphics_draw_glyph_scaled()
void graphics_draw_text(int x, int y, const char *s, uint32_t fg, int scale);

/**
 * Move the pixel rows [y_top + dy, y_bottom) up by dy rows and fill the freed
 * rows at the bottom with `fill` (used for console scrolling)
 */
void graphics_scroll_up(int y_top, int y_bottom, int dy, uint32_t fill);

// Filled circle and 1-pixel ellipse outline (splash screen art)
void graphics_fill_circle(int cx, int cy, int r, uint32_t rgb);
void graphics_draw_ellipse(int cx, int cy, int rx, int ry, uint32_t rgb, bool front_half_only);

// Linear interpolation between two RGB colors, t in [0, 255]
uint32_t graphics_blend(uint32_t a, uint32_t b, uint8_t t);

#endif
