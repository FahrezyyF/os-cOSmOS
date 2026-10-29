#include "header/graphics/graphics.h"
#include "header/cpu/portio.h"
#include "header/driver/pci.h"
#include "header/text/font.h"
#include "header/memory/paging.h"

const uint32_t graphics_vga_palette[16] = {
    RGB(0x00, 0x00, 0x00), RGB(0x00, 0x00, 0xAA), RGB(0x00, 0xAA, 0x00), RGB(0x00, 0xAA, 0xAA),
    RGB(0xAA, 0x00, 0x00), RGB(0xAA, 0x00, 0xAA), RGB(0xAA, 0x55, 0x00), RGB(0xAA, 0xAA, 0xAA),
    RGB(0x55, 0x55, 0x55), RGB(0x55, 0x55, 0xFF), RGB(0x55, 0xFF, 0x55), RGB(0x55, 0xFF, 0xFF),
    RGB(0xFF, 0x55, 0x55), RGB(0xFF, 0x55, 0xFF), RGB(0xFF, 0xFF, 0x55), RGB(0xFF, 0xFF, 0xFF),
};

static volatile uint32_t *lfb      = 0; // kernel virtual address (KERNEL_LFB_BASE)
static uint32_t          lfb_phys = 0; // PCI BAR0
static uint16_t          screen_w  = 0;
static uint16_t          screen_h  = 0;
static uint16_t          bga_id    = 0;
static bool              active    = false;

static void bga_write(uint16_t index, uint16_t value) {
    out16(BGA_INDEX_PORT, index);
    out16(BGA_DATA_PORT, value);
}

static uint16_t bga_read(uint16_t index) {
    out16(BGA_INDEX_PORT, index);
    return in16(BGA_DATA_PORT);
}

// rep stosl / rep movsl: one instruction per row instead of a C loop per pixel
static void fill32(volatile uint32_t *dst, uint32_t value, uint32_t count) {
    __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(count) : "a"(value) : "memory");
}

static void copy32_forward(volatile uint32_t *dst, const volatile uint32_t *src, uint32_t count) {
    __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
}

static uint32_t isqrt(uint32_t n) {
    uint32_t root = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit != 0) {
        if (n >= root + bit) {
            n    -= root + bit;
            root  = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static uint32_t find_lfb_address(void) {
    struct PCIAddress dev;
    if (pci_find_device(BGA_PCI_VENDOR, BGA_PCI_DEVICE, &dev) ||
        pci_find_device(BGA_VBOX_PCI_VENDOR, BGA_VBOX_PCI_DEVICE, &dev)) {
        uint32_t bar0 = pci_config_read32(dev, 0x10);
        // Bit 0 = 0 means memory BAR, the low 4 bits are flags
        if ((bar0 & 0x1) == 0 && (bar0 & 0xFFFFFFF0u) != 0)
            return bar0 & 0xFFFFFFF0u;
    }
    return BGA_DEFAULT_LFB_ADDRESS;
}

bool graphics_init(uint16_t width, uint16_t height) {
    // Writing the newest known id and reading it back yields the supported version
    bga_write(BGA_REG_ID, BGA_ID_MAX);
    bga_id = bga_read(BGA_REG_ID);
    if (bga_id < BGA_ID_MIN || bga_id > BGA_ID_MAX)
        return false;

    uint32_t lfb_address = find_lfb_address();
    uint32_t lfb_size    = (uint32_t) width * height * (GRAPHICS_BPP / 8);
    // With paging on, the linear framebuffer must be mapped into kernel space first
    if (lfb_size > KERNEL_LFB_MAX_SIZE || !paging_map_kernel_physical(KERNEL_LFB_BASE, lfb_address, lfb_size))
        return false;

    bga_write(BGA_REG_ENABLE, BGA_DISABLED);
    bga_write(BGA_REG_XRES, width);
    bga_write(BGA_REG_YRES, height);
    bga_write(BGA_REG_BPP, GRAPHICS_BPP);
    bga_write(BGA_REG_VIRT_WIDTH, width);
    bga_write(BGA_REG_X_OFFSET, 0);
    bga_write(BGA_REG_Y_OFFSET, 0);
    bga_write(BGA_REG_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);

    if (bga_read(BGA_REG_XRES) != width || bga_read(BGA_REG_YRES) != height ||
        bga_read(BGA_REG_BPP) != GRAPHICS_BPP || bga_read(BGA_REG_VIRT_WIDTH) != width) {
        bga_write(BGA_REG_ENABLE, BGA_DISABLED); // back to VGA text mode
        return false;
    }

    lfb      = (volatile uint32_t*) KERNEL_LFB_BASE;
    lfb_phys = lfb_address;
    screen_w = width;
    screen_h = height;
    active   = true;
    graphics_clear(RGB(0, 0, 0));
    return true;
}

bool graphics_is_active(void) {
    return active;
}

uint16_t graphics_get_width(void) {
    return screen_w;
}

uint16_t graphics_get_height(void) {
    return screen_h;
}

uint16_t graphics_get_bga_version(void) {
    return bga_id;
}

uint32_t graphics_get_lfb_address(void) {
    return lfb_phys;
}

void graphics_put_pixel(int x, int y, uint32_t rgb) {
    if (!active || x < 0 || y < 0 || x >= screen_w || y >= screen_h)
        return;
    lfb[(uint32_t) y * screen_w + (uint32_t) x] = rgb;
}

uint32_t graphics_get_pixel(int x, int y) {
    if (!active || x < 0 || y < 0 || x >= screen_w || y >= screen_h)
        return 0;
    return lfb[(uint32_t) y * screen_w + (uint32_t) x];
}

void graphics_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!active)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int row = 0; row < h; row++)
        fill32(lfb + (uint32_t) (y + row) * screen_w + (uint32_t) x, rgb, (uint32_t) w);
}

void graphics_clear(uint32_t rgb) {
    if (active)
        fill32(lfb, rgb, (uint32_t) screen_w * screen_h);
}

void graphics_draw_glyph(int x, int y, uint8_t ch, uint32_t fg, uint32_t bg) {
    if (!active)
        return;
    const uint8_t *glyph = font8x16[ch];
    if (x < 0 || y < 0 || x + FONT_WIDTH > screen_w || y + FONT_HEIGHT > screen_h) {
        for (int row = 0; row < FONT_HEIGHT; row++)
            for (int col = 0; col < FONT_WIDTH; col++)
                graphics_put_pixel(x + col, y + row, (glyph[row] & (0x80 >> col)) ? fg : bg);
        return;
    }
    volatile uint32_t *p = lfb + (uint32_t) y * screen_w + (uint32_t) x;
    for (int row = 0; row < FONT_HEIGHT; row++, p += screen_w) {
        uint8_t bits = glyph[row];
        p[0] = (bits & 0x80) ? fg : bg;
        p[1] = (bits & 0x40) ? fg : bg;
        p[2] = (bits & 0x20) ? fg : bg;
        p[3] = (bits & 0x10) ? fg : bg;
        p[4] = (bits & 0x08) ? fg : bg;
        p[5] = (bits & 0x04) ? fg : bg;
        p[6] = (bits & 0x02) ? fg : bg;
        p[7] = (bits & 0x01) ? fg : bg;
    }
}

void graphics_draw_glyph_scaled(int x, int y, uint8_t ch, uint32_t fg, int scale) {
    const uint8_t *glyph = font8x16[ch];
    for (int row = 0; row < FONT_HEIGHT; row++)
        for (int col = 0; col < FONT_WIDTH; col++)
            if (glyph[row] & (0x80 >> col))
                graphics_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
}

void graphics_draw_text(int x, int y, const char *s, uint32_t fg, int scale) {
    for (; *s != '\0'; s++, x += FONT_WIDTH * scale)
        graphics_draw_glyph_scaled(x, y, (uint8_t) *s, fg, scale);
}

void graphics_scroll_up(int y_top, int y_bottom, int dy, uint32_t fill) {
    if (!active || y_top < 0 || y_bottom > screen_h || dy <= 0 || y_top >= y_bottom)
        return;
    if (dy > y_bottom - y_top)
        dy = y_bottom - y_top;
    uint32_t moved_rows = (uint32_t) (y_bottom - y_top - dy);
    // Rows are contiguous because VIRT_WIDTH == XRES, so one forward copy moves the region
    copy32_forward(lfb + (uint32_t) y_top * screen_w, lfb + (uint32_t) (y_top + dy) * screen_w, moved_rows * screen_w);
    fill32(lfb + (uint32_t) (y_bottom - dy) * screen_w, fill, (uint32_t) dy * screen_w);
}

void graphics_fill_circle(int cx, int cy, int r, uint32_t rgb) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int) isqrt((uint32_t) (r * r - dy * dy));
        graphics_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, rgb);
    }
}

void graphics_draw_ellipse(int cx, int cy, int rx, int ry, uint32_t rgb, bool front_half_only) {
    // rx^2 * ry^2 must fit in 32 bits for the integer math below
    if (rx <= 0 || ry <= 0 || (uint32_t) rx * (uint32_t) ry > 65000u)
        return;
    uint32_t rx2 = (uint32_t) (rx * rx), ry2 = (uint32_t) (ry * ry);
    // Sample along x, then along y, so steep and flat parts are both gap-free
    for (int x = -rx; x <= rx; x++) {
        int y = (int) isqrt(ry2 * (rx2 - (uint32_t) (x * x)) / rx2);
        graphics_put_pixel(cx + x, cy + y, rgb);
        if (!front_half_only)
            graphics_put_pixel(cx + x, cy - y, rgb);
    }
    for (int y = -ry; y <= ry; y++) {
        if (front_half_only && y < 0)
            continue;
        int x = (int) isqrt(rx2 * (ry2 - (uint32_t) (y * y)) / ry2);
        graphics_put_pixel(cx + x, cy + y, rgb);
        graphics_put_pixel(cx - x, cy + y, rgb);
    }
}

uint32_t graphics_blend(uint32_t a, uint32_t b, uint8_t t) {
    uint32_t r  = (((a >> 16) & 0xFF) * (255u - t) + ((b >> 16) & 0xFF) * t) / 255u;
    uint32_t g  = (((a >> 8) & 0xFF) * (255u - t) + ((b >> 8) & 0xFF) * t) / 255u;
    uint32_t bl = ((a & 0xFF) * (255u - t) + (b & 0xFF) * t) / 255u;
    return RGB(r, g, bl);
}
