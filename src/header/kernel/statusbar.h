#ifndef _STATUSBAR_H
#define _STATUSBAR_H

#include <stdint.h>
#include <stdbool.h>

#define COSMOS_VERSION "v0.3.0"

/**
 * Status bar pinned to the top row of the screen (the console scrolls below it).
 * Left: OS version and the text set by the shell (root file system and working directory).
 * Right: scrollback position, memory / swap use, mouse and the RTC clock.
 */

// Reserve row 0 for the bar and restrict the console to the rows below it
void statusbar_init(void);

// SYS_SET_STATUS: replace the text shown after the version (copied, truncated to fit)
void statusbar_set_text(const char *text);

// Redraw now
void statusbar_draw(void);

/**
 * Background work while the kernel waits for the user (getchar / sleep system calls):
 * cursor blink, mouse pointer, mouse wheel scrolling of the console, status bar clock.
 */
void kernel_idle(void);

#endif
