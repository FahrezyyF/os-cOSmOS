#include "header/graphics/compositor.h"
#include "header/graphics/graphics.h"
#include "header/text/framebuffer.h"
#include "header/text/font.h"
#include "header/kernel/syscall.h"
#include "header/kernel/user.h"
#include "header/kernel/statusbar.h"
#include "header/process/process.h"
#include "header/process/scheduler.h"
#include "header/memory/kheap.h"
#include "header/stdlib/string.h"
#include "header/driver/serial.h"

#define TITLE_H   COMPOSITOR_TITLE_HEIGHT
#define BORDER    COMPOSITOR_BORDER
#define CLOSE_W   14

struct Window {
    bool     used;
    int32_t  id;
    uint32_t owner;
    int32_t  x, y;          // client area, screen pixels
    int32_t  w, h;
    char     title[SYSCALL_WINDOW_TITLE_MAX];
    uint32_t *pixels;       // kernel copy of the program's buffer
    struct SyscallWindowEvent events[COMPOSITOR_EVENT_MAX];
    uint32_t ev_head, ev_count;
    uint32_t presents;
};

static struct Window windows[COMPOSITOR_WINDOW_MAX];
static int32_t  stack_order[COMPOSITOR_WINDOW_MAX]; // window indexes, bottom first
static uint32_t stack_count  = 0;
static int32_t  focused      = -1;                  // window index with the keyboard, -1 = console
static int32_t  next_id      = 1;
static int32_t  drag_index   = -1;
static int32_t  drag_dx, drag_dy;
static uint8_t  last_buttons = 0;
static int32_t  cascade      = 0;

/* ================================ GEOMETRY ========================================= */

struct Rect {
    int32_t x, y, w, h;
};

static struct Rect frame_rect(const struct Window *win) {
    struct Rect r = {win->x - BORDER, win->y - TITLE_H - BORDER, win->w + 2 * BORDER, win->h + TITLE_H + 2 * BORDER};
    return r;
}

static bool intersect(struct Rect a, struct Rect b, struct Rect *out) {
    int32_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int32_t x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int32_t y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0)
        return false;
    if (out != NULL) {
        out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0;
    }
    return true;
}

static bool contains(struct Rect r, int32_t x, int32_t y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// The area windows may cover: between the status bar and the taskbar
static struct Rect desktop(void) {
    struct Rect r = {0, FONT_HEIGHT, graphics_get_width(), graphics_get_height() - 2 * FONT_HEIGHT};
    return r;
}

static void clamp_position(struct Window *win) {
    struct Rect d = desktop();
    int32_t min_x = d.x + BORDER, min_y = d.y + TITLE_H + BORDER;
    int32_t max_x = d.x + d.w - win->w - BORDER, max_y = d.y + d.h - win->h - BORDER;
    if (win->x > max_x) win->x = max_x;
    if (win->y > max_y) win->y = max_y;
    if (win->x < min_x) win->x = min_x;
    if (win->y < min_y) win->y = min_y;
}

/* ================================ PAINTING ========================================= */

static void fill_clipped(struct Rect area, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    struct Rect r = {x, y, w, h}, c;
    if (intersect(area, r, &c))
        graphics_fill_rect(c.x, c.y, c.w, c.h, rgb);
}

// One window clipped to `area` (decoration + client)
static void paint_window(const struct Window *win, int32_t index, struct Rect area) {
    struct Rect f = frame_rect(win), clip;
    if (!intersect(f, area, &clip))
        return;
    bool has_focus = index == focused;
    uint32_t border = has_focus ? RGB(0x40, 0x80, 0xE0) : RGB(0x70, 0x70, 0x70);
    uint32_t title  = has_focus ? RGB(0x20, 0x50, 0xB0) : RGB(0x48, 0x48, 0x48);
    // border and title bar
    fill_clipped(clip, f.x, f.y, f.w, BORDER, border);
    fill_clipped(clip, f.x, f.y + f.h - BORDER, f.w, BORDER, border);
    fill_clipped(clip, f.x, f.y, BORDER, f.h, border);
    fill_clipped(clip, f.x + f.w - BORDER, f.y, BORDER, f.h, border);
    fill_clipped(clip, win->x, win->y - TITLE_H, win->w, TITLE_H, title);
    // close box with its X, and the title (glyph pixels clipped one by one)
    int32_t cx = win->x + win->w - CLOSE_W - 3, cy = win->y - TITLE_H + 3;
    fill_clipped(clip, cx, cy, CLOSE_W, CLOSE_W, RGB(0xC0, 0x30, 0x30));
    for (int32_t k = 3; k < CLOSE_W - 3; k++) {
        if (contains(clip, cx + k, cy + k))
            graphics_put_pixel(cx + k, cy + k, RGB(255, 255, 255));
        if (contains(clip, cx + CLOSE_W - 1 - k, cy + k))
            graphics_put_pixel(cx + CLOSE_W - 1 - k, cy + k, RGB(255, 255, 255));
    }
    struct Rect bar = {win->x, win->y - TITLE_H, win->w, TITLE_H};
    if (intersect(bar, clip, NULL)) {
        int32_t max_chars = (win->w - CLOSE_W - 12) / FONT_WIDTH;
        int32_t tx = win->x + 6, ty = win->y - TITLE_H + 2;
        for (int32_t n = 0; win->title[n] != '\0' && n < max_chars; n++, tx += FONT_WIDTH) {
            const uint8_t *glyph = font8x16[(uint8_t) win->title[n]];
            for (int32_t gy = 0; gy < FONT_HEIGHT; gy++)
                for (int32_t gx = 0; gx < FONT_WIDTH; gx++)
                    if ((glyph[gy] & (0x80 >> gx)) && contains(clip, tx + gx, ty + gy))
                        graphics_put_pixel(tx + gx, ty + gy, RGB(255, 255, 255));
        }
    }
    // client pixels
    struct Rect client = {win->x, win->y, win->w, win->h}, cc;
    if (!intersect(client, clip, &cc))
        return;
    for (int32_t row = cc.y; row < cc.y + cc.h; row++) {
        const uint32_t *src = &win->pixels[(row - win->y) * win->w + (cc.x - win->x)];
        graphics_blit_row(cc.x, row, src, cc.w);
    }
}

// Compose `area`: every window that touches it, bottom to top (the console is already there)
static void repaint_windows(struct Rect area) {
    if (stack_count == 0)
        return;
    bool pointer = framebuffer_pointer_hide_over(area.x, area.y, area.w, area.h);
    for (uint32_t k = 0; k < stack_count; k++) {
        int32_t i = stack_order[k];
        paint_window(&windows[i], i, area);
    }
    if (pointer)
        framebuffer_pointer_show();
}

// Console first (uncovered parts), then the windows: used when a window moves or closes
static void repaint_all(struct Rect area) {
    // The console repaints whole 8x16 cells: widen the area to cell boundaries so the
    // windows are painted back over every pixel the console touched
    int32_t x0 = area.x / FONT_WIDTH * FONT_WIDTH, y0 = area.y / FONT_HEIGHT * FONT_HEIGHT;
    int32_t x1 = (area.x + area.w + FONT_WIDTH - 1) / FONT_WIDTH * FONT_WIDTH;
    int32_t y1 = (area.y + area.h + FONT_HEIGHT - 1) / FONT_HEIGHT * FONT_HEIGHT;
    struct Rect cells = {x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, x1 - (x0 < 0 ? 0 : x0), y1 - (y0 < 0 ? 0 : y0)};
    framebuffer_redraw_pixels(cells.x, cells.y, cells.w, cells.h);
    repaint_windows(cells);
}

void compositor_after_console_scroll(int y_top, int y_bottom, int dy) {
    if (stack_count == 0)
        return;
    struct Rect band = {0, y_top, graphics_get_width(), y_bottom - y_top};
    for (uint32_t k = 0; k < stack_count; k++) {
        struct Rect f = frame_rect(&windows[stack_order[k]]);
        struct Rect moved = {f.x, f.y - dy, f.w, f.h + dy}, c;
        if (intersect(moved, band, &c))
            repaint_all(c); // console cells from the shadow grid, then every window on top
    }
}

void compositor_after_console_draw(int x, int y, int w, int h) {
    if (stack_count == 0)
        return;
    struct Rect area = {x, y, w, h};
    for (uint32_t k = 0; k < stack_count; k++) {
        struct Rect f = frame_rect(&windows[stack_order[k]]), c;
        if (intersect(f, area, &c)) {
            repaint_windows(c);
        }
    }
}

/* ================================ STACKING / FOCUS ================================= */

static void stack_remove(int32_t index) {
    for (uint32_t k = 0; k < stack_count; k++) {
        if (stack_order[k] == index) {
            for (uint32_t j = k; j + 1 < stack_count; j++)
                stack_order[j] = stack_order[j + 1];
            stack_count--;
            return;
        }
    }
}

static void push_event(struct Window *win, uint8_t type, char key, uint8_t buttons, int32_t x, int32_t y) {
    if (win->ev_count == COMPOSITOR_EVENT_MAX) { // full: drop the oldest
        win->ev_head = (win->ev_head + 1) % COMPOSITOR_EVENT_MAX;
        win->ev_count--;
    }
    struct SyscallWindowEvent *e = &win->events[(win->ev_head + win->ev_count) % COMPOSITOR_EVENT_MAX];
    e->type    = type;
    e->key     = key;
    e->buttons = buttons;
    e->x       = x;
    e->y       = y;
    e->scancode = 0;
    e->extended = false;
    win->ev_count++;
    scheduler_request_resched();
}

static void focus_window(int32_t index) {
    int32_t old = focused;
    focused = index;
    if (index >= 0) { // raise
        stack_remove(index);
        stack_order[stack_count++] = index;
        push_event(&windows[index], SYSCALL_WEVENT_FOCUS, 1, 0, 0, 0);
    }
    if (old >= 0 && old != index && windows[old].used) {
        push_event(&windows[old], SYSCALL_WEVENT_FOCUS, 0, 0, 0, 0);
        repaint_windows(frame_rect(&windows[old]));
    }
    if (index >= 0)
        repaint_windows(frame_rect(&windows[index]));
    statusbar_mark_dirty();
}

bool compositor_has_keyboard_focus(void) {
    return focused >= 0;
}

void compositor_raw_key_hook(uint8_t scancode, bool extended, bool released) {
    if (focused < 0 || !windows[focused].used)
        return;
    struct Window *win = &windows[focused];
    push_event(win, released ? SYSCALL_WEVENT_KEY_UP : SYSCALL_WEVENT_KEY_DOWN, 0, 0, 0, 0);
    struct SyscallWindowEvent *e = &win->events[(win->ev_head + win->ev_count - 1) % COMPOSITOR_EVENT_MAX];
    e->scancode = scancode;
    e->extended = extended;
}

bool compositor_key_hook(char c) {
    if (focused < 0 || !windows[focused].used)
        return false;
    push_event(&windows[focused], SYSCALL_WEVENT_KEY, c, 0, 0, 0);
    return true;
}

bool compositor_has_event(uint32_t pid) {
    for (int32_t i = 0; i < COMPOSITOR_WINDOW_MAX; i++)
        if (windows[i].used && windows[i].owner == pid && windows[i].ev_count > 0)
            return true;
    return false;
}

/* ================================ WINDOW LIFETIME ================================== */

static void destroy_window(int32_t index) {
    struct Window *win = &windows[index];
    if (!win->used)
        return;
    struct Rect f = frame_rect(win);
    stack_remove(index);
    kfree(win->pixels);
    memset(win, 0, sizeof(*win));
    if (focused == index)
        focused = -1; // the keyboard goes back to the console
    if (drag_index == index)
        drag_index = -1;
    repaint_all(f);
    statusbar_mark_dirty();
}

void compositor_on_process_exit(uint32_t pid) {
    for (int32_t i = 0; i < COMPOSITOR_WINDOW_MAX; i++)
        if (windows[i].used && windows[i].owner == pid)
            destroy_window(i);
}

static int32_t find_window(int32_t id, uint32_t owner) {
    for (int32_t i = 0; i < COMPOSITOR_WINDOW_MAX; i++)
        if (windows[i].used && windows[i].id == id && windows[i].owner == owner)
            return i;
    return -1;
}

static void move_window(int32_t index, int32_t x, int32_t y) {
    struct Window *win = &windows[index];
    struct Rect old = frame_rect(win);
    win->x = x;
    win->y = y;
    clamp_position(win);
    struct Rect now = frame_rect(win);
    if (old.x == now.x && old.y == now.y)
        return;
    // Uncover the old place (console + lower windows), then paint at the new place
    repaint_all(old);
    repaint_windows(now);
}

/* ================================ MOUSE ============================================ */

static int32_t window_at(int32_t x, int32_t y) {
    for (int32_t k = (int32_t) stack_count - 1; k >= 0; k--)
        if (contains(frame_rect(&windows[stack_order[k]]), x, y))
            return stack_order[k];
    return -1;
}

bool compositor_mouse_event(const struct MouseState *m) {
    if (!graphics_is_active())
        return false;
    uint8_t pressed  = (uint8_t) (m->buttons & ~last_buttons);
    uint8_t released = (uint8_t) (~m->buttons & last_buttons);
    last_buttons = m->buttons;

    if (drag_index >= 0) {
        if (m->buttons & MOUSE_BUTTON_LEFT) {
            move_window(drag_index, m->x - drag_dx, m->y - drag_dy);
            return true;
        }
        drag_index = -1;
        return true;
    }
    int32_t index = window_at(m->x, m->y);
    if (index < 0) {
        if (pressed && focused >= 0) // click on the console: keyboard back to the shell
            focus_window(-1);
        return false;
    }
    struct Window *win = &windows[index];
    int32_t rx = m->x - win->x, ry = m->y - win->y;
    if (pressed) {
        if (index != focused || stack_order[stack_count - 1] != index)
            focus_window(index);
        struct Rect close = {win->x + win->w - CLOSE_W - 3, win->y - TITLE_H + 3, CLOSE_W, CLOSE_W};
        struct Rect bar   = {win->x - BORDER, win->y - TITLE_H - BORDER, win->w + 2 * BORDER, TITLE_H + BORDER};
        if (contains(close, m->x, m->y)) {
            push_event(win, SYSCALL_WEVENT_CLOSE, 0, m->buttons, 0, 0);
        } else if (contains(bar, m->x, m->y) && (pressed & MOUSE_BUTTON_LEFT)) {
            drag_index = index;
            drag_dx    = m->x - win->x;
            drag_dy    = m->y - win->y;
        } else if (rx >= 0 && ry >= 0 && rx < win->w && ry < win->h) {
            push_event(win, SYSCALL_WEVENT_MOUSE_DOWN, 0, m->buttons, rx, ry);
        }
        return true;
    }
    if (rx >= 0 && ry >= 0 && rx < win->w && ry < win->h) {
        if (released)
            push_event(win, SYSCALL_WEVENT_MOUSE_UP, 0, m->buttons, rx, ry);
        else if (index == focused)
            push_event(win, SYSCALL_WEVENT_MOUSE_MOVE, 0, m->buttons, rx, ry);
    }
    return m->wheel == 0 || index >= 0;
}

void compositor_tick(uint32_t now_ms) {
    (void) now_ms;
}

/* ================================ SYSTEM CALLS ===================================== */

static void sys_create(uint32_t user_ptr, uint32_t result_ptr) {
    int32_t result = -1;
    struct ProcessControlBlock *me = process_get_current_running_pcb_pointer();
    struct SyscallWindow req;
    if (!graphics_is_active() || me == NULL || !user_range_is_valid((void*) user_ptr, sizeof(req)))
        goto done;
    memcpy(&req, (void*) user_ptr, sizeof(req));
    if (req.width < COMPOSITOR_MIN_SIZE || req.height < COMPOSITOR_MIN_SIZE || req.width > COMPOSITOR_MAX_WIDTH ||
        req.height > COMPOSITOR_MAX_HEIGHT)
        goto done;
    int32_t index = -1;
    for (int32_t i = 0; i < COMPOSITOR_WINDOW_MAX; i++)
        if (!windows[i].used) {
            index = i;
            break;
        }
    if (index < 0)
        goto done;
    uint32_t *pixels = kmalloc(req.width * req.height * sizeof(uint32_t));
    if (pixels == NULL)
        goto done;
    struct Window *win = &windows[index];
    memset(win, 0, sizeof(*win));
    win->used   = true;
    win->id     = next_id++;
    win->owner  = me->metadata.pid;
    win->w      = (int32_t) req.width;
    win->h      = (int32_t) req.height;
    win->pixels = pixels;
    for (uint32_t i = 0; i < req.width * req.height; i++)
        pixels[i] = RGB(0x10, 0x10, 0x18);
    memcpy(win->title, req.title, sizeof(win->title));
    win->title[sizeof(win->title) - 1] = '\0';
    struct Rect d = desktop();
    if (req.x < 0 || req.y < 0) { // cascade from the top right
        win->x = d.x + d.w - win->w - 40 - cascade * 30;
        win->y = d.y + TITLE_H + 30 + cascade * 30;
        cascade = (cascade + 1) % 6;
    } else {
        win->x = req.x;
        win->y = req.y;
    }
    clamp_position(win);
    // On top, but the keyboard stays with the console until the window is clicked (unless the
    // program asks for the focus: a game or player that the shell waits for)
    stack_order[stack_count++] = index;
    if (req.flags & SYSCALL_WINDOW_FOCUS)
        focus_window(index);
    else
        repaint_windows(frame_rect(win));
    result = win->id;
    serial_printf("[wm] window %d \"%s\" %dx%d for pid %u\n", win->id, win->title, win->w, win->h, win->owner);
done:
    if (user_range_is_valid((void*) result_ptr, sizeof(result)))
        memcpy((void*) result_ptr, &result, sizeof(result));
}

static void sys_present(uint32_t id, uint32_t pixels_ptr) {
    struct ProcessControlBlock *me = process_get_current_running_pcb_pointer();
    int32_t index = me ? find_window((int32_t) id, me->metadata.pid) : -1;
    if (index < 0)
        return;
    struct Window *win = &windows[index];
    uint32_t bytes = (uint32_t) (win->w * win->h) * sizeof(uint32_t);
    if (!user_range_is_valid((void*) pixels_ptr, bytes))
        return;
    memcpy(win->pixels, (void*) pixels_ptr, bytes); // the program's buffer -> the window's copy
    win->presents++;
    struct Rect client = {win->x, win->y, win->w, win->h};
    repaint_windows(client); // windows above it are painted again on top
}

static void sys_event(const struct InterruptFrame *frame, uint32_t id, uint32_t event_ptr, uint32_t block) {
    struct ProcessControlBlock *me = process_get_current_running_pcb_pointer();
    int32_t index = me ? find_window((int32_t) id, me->metadata.pid) : -1;
    struct SyscallWindowEvent e;
    memset(&e, 0, sizeof(e));
    if (index >= 0) {
        struct Window *win = &windows[index];
        if (win->ev_count > 0) {
            e = win->events[win->ev_head];
            win->ev_head = (win->ev_head + 1) % COMPOSITOR_EVENT_MAX;
            win->ev_count--;
        } else if (block) {
            scheduler_block_current(frame, PROCESS_WAIT_WINDOW, id, true);
        }
    } else {
        e.type = SYSCALL_WEVENT_CLOSE; // unknown window: tell the program to stop
    }
    if (user_range_is_valid((void*) event_ptr, sizeof(e)))
        memcpy((void*) event_ptr, &e, sizeof(e));
}

void compositor_syscall(const struct InterruptFrame *frame, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    struct ProcessControlBlock *me = process_get_current_running_pcb_pointer();
    switch (eax) {
        case SYS_WIN_CREATE:
            sys_create(ebx, ecx);
            break;
        case SYS_WIN_PRESENT:
            sys_present(ebx, ecx);
            break;
        case SYS_WIN_EVENT:
            sys_event(frame, ebx, ecx, edx);
            break;
        case SYS_WIN_DESTROY: {
            int32_t index = me ? find_window((int32_t) ebx, me->metadata.pid) : -1;
            if (index >= 0)
                destroy_window(index);
            break;
        }
        case SYS_WIN_SET_TITLE: {
            int32_t index = me ? find_window((int32_t) ebx, me->metadata.pid) : -1;
            if (index >= 0 && user_string_is_valid((const char*) ecx, SYSCALL_WINDOW_TITLE_MAX - 1)) {
                strncpy(windows[index].title, (const char*) ecx, SYSCALL_WINDOW_TITLE_MAX - 1);
                repaint_windows(frame_rect(&windows[index]));
            }
            break;
        }
        case SYS_WIN_INFO: {
            struct SyscallWindowInfo info = {
                .available     = graphics_is_active(),
                .screen_width  = graphics_is_active() ? graphics_get_width() : 0,
                .screen_height = graphics_is_active() ? graphics_get_height() : 0,
                .windows       = stack_count,
                .windows_max   = COMPOSITOR_WINDOW_MAX,
            };
            if (user_range_is_valid((void*) ebx, sizeof(info)))
                memcpy((void*) ebx, &info, sizeof(info));
            break;
        }
        default:
            break;
    }
}
