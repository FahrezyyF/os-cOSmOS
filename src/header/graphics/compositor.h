#ifndef _COMPOSITOR_H
#define _COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "header/driver/mouse.h"
#include "header/cpu/interrupt.h"

/**
 * Window compositor (bonus), graphics mode only.
 *
 * Every program may own windows. The program draws into its OWN pixel buffer (user memory,
 * 0x00RRGGBB) and hands it to the kernel with SYS_WIN_PRESENT; the kernel keeps a copy per
 * window and composes the screen: console (the desktop) at the bottom, windows on top in
 * stacking order, the mouse pointer last. Windows have a title bar (drag to move), a close
 * box (sends a CLOSE event), and get keyboard / mouse events while they have the focus
 * (click into a window to focus it, click on the console to give the keyboard back).
 */

#define COMPOSITOR_WINDOW_MAX   8
#define COMPOSITOR_EVENT_MAX    64
#define COMPOSITOR_MIN_SIZE     32
#define COMPOSITOR_MAX_WIDTH    800
#define COMPOSITOR_MAX_HEIGHT   600
#define COMPOSITOR_TITLE_HEIGHT 20
#define COMPOSITOR_BORDER       2

// Keyboard focus is on a window: the console (foreground process) does not get keys
bool compositor_has_keyboard_focus(void);

// Keyboard ISR hook: a focused window takes the key. @return true when consumed
bool compositor_key_hook(char c);

// A window of this process has a pending event (wakes PROCESS_WAIT_WINDOW)
bool compositor_has_event(uint32_t pid);

// The process ended: close its windows
void compositor_on_process_exit(uint32_t pid);

/**
 * Mouse hook (kernel_idle): focus, raise, drag, close box, events into windows.
 * @return true when the window manager used the event (the console ignores it then)
 */
bool compositor_mouse_event(const struct MouseState *m);

// Periodic work at safe points (nothing time critical)
void compositor_tick(uint32_t now_ms);

/**
 * The console drew into this pixel rectangle: paint back the windows covering it.
 * Called by the framebuffer driver after every cell / scroll / clear in graphics mode.
 */
void compositor_after_console_draw(int x, int y, int w, int h);

// SYS_WIN_* dispatcher
void compositor_syscall(const struct InterruptFrame *frame, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

#endif
