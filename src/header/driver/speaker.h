#ifndef _SPEAKER_H
#define _SPEAKER_H

#include <stdint.h>

#define SPEAKER_CONTROL_PORT 0x61

/**
 * PC speaker driver (bonus: Hardware Drivers - Speaker).
 * The speaker is driven by PIT channel 2 in square-wave mode; port 0x61 bits 0-1
 * gate the timer output to the speaker cone.
 * In QEMU, run with `make run-sound` to hear it.
 */

// Start a continuous tone (frequency in Hz, 0 = stop)
void speaker_tone(uint32_t frequency_hz);

// Silence the speaker
void speaker_stop(void);

// Blocking beep using timer_sleep_ms()
void speaker_beep(uint32_t frequency_hz, uint32_t duration_ms);

// Short ascending chime played at boot
void speaker_play_startup_chime(void);

#endif
