#include "header/kernel/statusbar.h"
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/driver/timer.h"
#include "header/driver/mouse.h"
#include "header/memory/paging.h"
#include "header/memory/swap.h"
#include "header/process/process.h"
#include "header/graphics/compositor.h"
#include "header/stdlib/string.h"
#include "header/stdlib/format.h"

#define STATUS_TEXT_MAX   128
#define WHEEL_LINES       3     // console lines scrolled per wheel notch

static bool              enabled = false;
static char              status_text[STATUS_TEXT_MAX + 1] = "";
static struct MouseState mouse_view;
static uint32_t          last_draw_ms = 0;
static uint32_t          last_offset  = 0;
static volatile bool     dirty        = true;

static struct {
    uint32_t pid;
    char     text[STATUSBAR_WIDGET_TEXT_MAX + 1];
    uint8_t  color;
} widgets[STATUSBAR_WIDGET_MAX];

void statusbar_init(void) {
    enabled = true;
    console_set_region(1, framebuffer_get_rows() - 1);
    statusbar_draw();
}

void statusbar_set_text(const char *text) {
    strncpy(status_text, text, STATUS_TEXT_MAX);
    status_text[STATUS_TEXT_MAX] = '\0';
    dirty = true;
}

void statusbar_mark_dirty(void) {
    dirty = true;
}

bool statusbar_set_widget(uint32_t pid, const char *text, uint8_t color) {
    int32_t slot = -1;
    for (int32_t i = 0; i < STATUSBAR_WIDGET_MAX; i++) {
        if (widgets[i].pid == pid) {
            slot = i;
            break;
        }
        if (widgets[i].pid == 0 && slot < 0)
            slot = i;
    }
    if (text[0] == '\0') {
        statusbar_remove_widget(pid);
        return true;
    }
    if (slot < 0)
        return false;
    widgets[slot].pid = pid;
    strncpy(widgets[slot].text, text, STATUSBAR_WIDGET_TEXT_MAX);
    widgets[slot].text[STATUSBAR_WIDGET_TEXT_MAX] = '\0';
    widgets[slot].color = color == 0 ? (COLOR_WHITE | COLOR_BLUE << 4) : color;
    dirty = true;
    return true;
}

void statusbar_remove_widget(uint32_t pid) {
    for (int32_t i = 0; i < STATUSBAR_WIDGET_MAX; i++) {
        if (widgets[i].pid == pid) {
            widgets[i].pid     = 0;
            widgets[i].text[0] = '\0';
            dirty = true;
        }
    }
}

static uint32_t percent(uint32_t part, uint32_t whole) {
    return whole == 0 ? 0 : part * 100 / whole; // frame / slot counts: no 32-bit overflow
}

static void draw_top(void) {
    uint8_t cols = framebuffer_get_cols();
    char left[STATUS_TEXT_MAX + 32], right[160], scroll[32] = "", mem[48];

    uint32_t usable = page_manager_state.total_page_frame_count - KERNEL_RESERVED_PAGE_FRAME_COUNT;
    uint32_t used   = usable - page_manager_state.free_page_frame_count;
    struct SwapStats swap;
    swap_get_stats(&swap);
    if (swap.available)
        snprintf(mem, sizeof(mem), " ram %u%% swap %u%% |", percent(used, usable), percent(swap.used_slots, swap.total_slots));
    else
        snprintf(mem, sizeof(mem), " ram %u%% |", percent(used, usable));

    uint32_t offset = console_view_offset();
    if (offset > 0)
        snprintf(scroll, sizeof(scroll), " [scrollback -%u/%u] |", offset, console_history_lines());

    uint32_t up = timer_get_ms() / 1000;
    snprintf(left, sizeof(left), " cOSmOS " COSMOS_VERSION " | %s ", status_text);
    if (cols < 100) // 80-column text mode: compact
        snprintf(right, sizeof(right), "%s procs %u |%s up %02u:%02u:%02u ", scroll,
                 process_manager_state.active_process_count, mem, up / 3600, up / 60 % 60, up % 60);
    else if (mouse_is_present())
        snprintf(right, sizeof(right), "%s %u processes |%s mouse %d,%d [%c%c%c] | up %02u:%02u:%02u ",
                 scroll, process_manager_state.active_process_count, mem, (int) mouse_view.x, (int) mouse_view.y,
                 (mouse_view.buttons & MOUSE_BUTTON_LEFT) ? 'L' : '-',
                 (mouse_view.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-',
                 (mouse_view.buttons & MOUSE_BUTTON_RIGHT) ? 'R' : '-',
                 up / 3600, up / 60 % 60, up % 60);
    else
        snprintf(right, sizeof(right), "%s %u processes |%s up %02u:%02u:%02u ", scroll,
                 process_manager_state.active_process_count, mem, up / 3600, up / 60 % 60, up % 60);

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

// Bottom row: process chips on the left, widgets (clock, ...) on the right
static void draw_taskbar(void) {
    uint8_t cols = framebuffer_get_cols();
    uint8_t row  = framebuffer_get_rows() - 1;
    uint8_t ch_buf[FRAMEBUFFER_MAX_COLS];
    uint8_t at_buf[FRAMEBUFFER_MAX_COLS];
    uint8_t base = COLOR_LIGHT_GRAY | COLOR_DARK_GRAY << 4;
    for (uint8_t c = 0; c < cols; c++) {
        ch_buf[c] = ' ';
        at_buf[c] = base;
    }

    // Right side first: widgets, newest rightmost
    int32_t right = cols;
    for (int32_t i = STATUSBAR_WIDGET_MAX - 1; i >= 0; i--) {
        if (widgets[i].pid == 0)
            continue;
        int32_t len = (int32_t) strlen(widgets[i].text) + 2;
        if (right - len < cols / 3)
            break;
        right -= len;
        for (int32_t k = 0; k < len; k++) {
            char t = (k == 0 || k == len - 1) ? ' ' : widgets[i].text[k - 1];
            ch_buf[right + k] = (uint8_t) t;
            at_buf[right + k] = widgets[i].color;
        }
        right--; // gap
    }

    // Left side: " tasks " then one chip per process
    const char *label = " tasks ";
    int32_t pos = 0;
    for (int32_t k = 0; label[k] && pos < right; k++, pos++) {
        ch_buf[pos] = (uint8_t) label[k];
        at_buf[pos] = COLOR_BLACK | COLOR_LIGHT_GRAY << 4;
    }
    pos++;
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        const struct ProcessControlBlock *p = &_process_list[i];
        if (p->metadata.state == PROCESS_STATE_INACTIVE || p->metadata.name[0] == '\0')
            continue;
        char chip[48];
        snprintf(chip, sizeof(chip), "%u %s", p->metadata.pid, p->metadata.name);
        int32_t len = (int32_t) strlen(chip) + 2;
        if (pos + len >= right)
            break;
        bool fg = p->metadata.pid == process_manager_state.foreground_pid;
        uint8_t attr = fg ? (COLOR_BLACK | COLOR_LIGHT_GREEN << 4)
                          : (p->metadata.state == PROCESS_STATE_BLOCKED ? (COLOR_LIGHT_GRAY | COLOR_BLACK << 4)
                                                                         : (COLOR_WHITE | COLOR_BLUE << 4));
        for (int32_t k = 0; k < len; k++) {
            ch_buf[pos + k] = (uint8_t) ((k == 0 || k == len - 1) ? ' ' : chip[k - 1]);
            at_buf[pos + k] = attr;
        }
        pos += len + 1;
    }

    for (uint8_t c = 0; c < cols; c++)
        framebuffer_write(row, c, (char) ch_buf[c], at_buf[c] & 0xF, at_buf[c] >> 4);
}

void statusbar_draw(void) {
    if (!enabled)
        return;
    draw_top();
    draw_taskbar();
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
            // The window manager gets the mouse first (drag, focus, clicks into windows)
            bool used = compositor_mouse_event(&m);
            if (!used && m.wheel != 0) // wheel down (positive) scrolls towards the live screen
                console_scroll_view(m.wheel * WHEEL_LINES);
            dirty = true;
        }
    }
    compositor_tick(now);
    if (dirty || console_view_offset() != last_offset || now - last_draw_ms >= 1000) {
        dirty        = false;
        statusbar_draw();
        last_draw_ms = now;
    }
}
