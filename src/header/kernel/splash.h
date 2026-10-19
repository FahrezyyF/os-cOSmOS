#ifndef _SPLASH_H
#define _SPLASH_H

#include <stdint.h>

/**
 * Boot splash screen: a planet with a ring, a starfield and a progress bar in
 * graphics mode; an ASCII-art banner in VGA text mode.
 */
void splash_begin(void);

// Advance the progress bar and show what the kernel is doing (percent 0-100)
void splash_step(const char *message, uint8_t percent);

// Keep the finished splash on screen for `hold_ms`, then clear the screen
void splash_end(uint32_t hold_ms);

#endif
