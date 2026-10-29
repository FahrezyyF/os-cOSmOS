#include "header/kernel/statusbar.h"
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/driver/timer.h"
#include "header/driver/mouse.h"
#include "header/driver/rtc.h"
#include "header/memory/paging.h"
#include "header/memory/swap.h"
#include "header/stdlib/string.h"
#include "header/stdlib/format.h"

#define STATUS_TEXT_MAX   128
#define WHEEL_LINES       3     // console lines scrolled per wheel notch

static bool              enabled = false;
static char              status_text[STATUS_TEXT_MAX + 1] = "";
static struct MouseState mouse_view;
static uint32_t          last_draw_ms = 0;
static uint32_t          last_offset  = 0;
static bool              dirty        = true;

void statusbar_init(void) {
    enabled = true;
    console_set_region(1, framebuffer_get_rows());
    statusbar_draw();
}

void statusbar_set_text(const char *text) {
    strncpy(status_text, text, STATUS_TEXT_MAX);
    status_text[STATUS_TEXT_MAX] = '\0';
    dirty = true;
}

static uint32_t percent(uint32_t part, uint32_t whole) {
    return whole == 0 ? 0 : part * 100 / whole; // frame / slot counts: no 32-bit overflow
}

void statusbar_draw(void) {
    if (!enabled)
        return;
    uint8_t cols = framebuffer_get_cols();
    char left[STATUS_TEXT_MAX + 32], right[128], scroll[32] = "", mem[48];
    struct RTCTime t;
    rtc_read(&t);

    uint32_t usable    = page_manager_state.total_page_frame_count - KERNEL_RESERVED_PAGE_FRAME_COUNT;
    uint32_t used      = usable - page_manager_state.free_page_frame_count;
    struct SwapStats swap;
    swap_get_stats(&swap);
    if (swap.available)
        snprintf(mem, sizeof(mem), " ram %u%% swap %u%% |", percent(used, usable), percent(swap.used_slots, swap.total_slots));
    else
        snprintf(mem, sizeof(mem), " ram %u%% |", percent(used, usable));

    uint32_t offset = console_view_offset();
    if (offset > 0)
        snprintf(scroll, sizeof(scroll), " [scrollback -%u/%u] |", offset, console_history_lines());

    snprintf(left, sizeof(left), " cOSmOS " COSMOS_VERSION " | %s ", status_text);
    if (cols < 100) // 80-column text mode: compact
        snprintf(right, sizeof(right), "%s%s %02u:%02u:%02u ", scroll, mem, t.hour, t.minute, t.second);
    else if (mouse_is_present())
        snprintf(right, sizeof(right), "%s%s mouse %d,%d [%c%c%c] | %04u-%02u-%02u %02u:%02u:%02u UTC ",
                 scroll, mem, (int) mouse_view.x, (int) mouse_view.y,
                 (mouse_view.buttons & MOUSE_BUTTON_LEFT) ? 'L' : '-',
                 (mouse_view.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-',
                 (mouse_view.buttons & MOUSE_BUTTON_RIGHT) ? 'R' : '-',
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
    else
        snprintf(right, sizeof(right), "%s%s %04u-%02u-%02u %02u:%02u:%02u UTC ",
                 scroll, mem, t.year, t.month, t.day, t.hour, t.minute, t.second);

    uint32_t llen = (uint32_t) strlen(left), rlen = (uint32_t) strlen(right);
    if (llen + rlen > cols && rlen < cols)
        llen = cols - rlen; // the right part wins, the path is cut
    for (uint8_t c = 0; c < cols; c++) {
        char    ch = ' ';
        uint8_t bg = COLOR_BLUE;
        if (c < llen) {
            ch = left[c];
            bg = COLOR_CYAN;
        } else if (c + rlen >= cols && c - (cols - rlen) < rlen) {
            ch = right[c - (cols - rlen)];
        }
        framebuffer_write(0, c, ch, COLOR_WHITE, bg);
    }
    last_offset = offset;
}

void kernel_idle(void) {
    uint32_t now = timer_get_ms();
    framebuffer_cursor_tick(now);
    if (mouse_is_present()) {
        struct MouseState m;
        mouse_get_state(&m);
        if (m.changed) {
            mouse_view = m;
            framebuffer_pointer_move(m.x, m.y);
            if (m.wheel != 0) // wheel down (positive) scrolls towards the live screen
                console_scroll_view(m.wheel * WHEEL_LINES);
            dirty = true;
        }
    }
    if (dirty || console_view_offset() != last_offset || now - last_draw_ms >= 1000) {
        statusbar_draw();
        last_draw_ms = now;
        dirty        = false;
    }
}
