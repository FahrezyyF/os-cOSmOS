#ifndef _STATUSBAR_H
#define _STATUSBAR_H

#include <stdint.h>
#include <stdbool.h>

#define COSMOS_VERSION "v1.0.0"

/**
 * Two kernel-drawn bars frame the console:
 *
 *   row 0         status bar: OS version, text set by the shell (file system, working
 *                 directory), scrollback position, process count, RAM / swap use, uptime
 *   rows 1..n-2   console (scrolls)
 *   row n-1       taskbar: one chip per process ("[1 shell]", the foreground one
 *                 highlighted) on the left, widgets of user programs on the right (the
 *                 `clock` program shows the time there, book 4.3.3)
 */

#define STATUSBAR_WIDGET_MAX      8
#define STATUSBAR_WIDGET_TEXT_MAX 40

// Reserve the top and bottom rows and restrict the console to the rows in between
void statusbar_init(void);

// SYS_SET_STATUS: replace the text shown after the version (copied, truncated to fit)
void statusbar_set_text(const char *text);

// Redraw both bars now
void statusbar_draw(void);

// Something changed (process list, foreground): redraw at the next safe point
void statusbar_mark_dirty(void);

/**
 * SYS_WIDGET_SET: show `text` for process `pid` on the right side of the taskbar
 * (empty text removes it). @return false when all widget slots are taken
 */
bool statusbar_set_widget(uint32_t pid, const char *text, uint8_t color);
void statusbar_remove_widget(uint32_t pid);

/**
 * Background work at safe points (idle loop, waiting system calls, timer ticks from user
 * mode): cursor blink, mouse pointer, wheel scrolling, window manager, bars.
 */
void kernel_idle(void);

#endif
