#include "header/driver/speaker.h"
#include "header/driver/timer.h"
#include "header/cpu/portio.h"

void speaker_tone(uint32_t frequency_hz) {
    if (frequency_hz < 20) {
        speaker_stop();
        return;
    }
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;
    out(PIT_COMMAND, 0xB6); // Channel 2, lobyte/hibyte, mode 3 (square wave)
    out(PIT_CHANNEL2_DATA, (uint8_t) (divisor & 0xFF));
    out(PIT_CHANNEL2_DATA, (uint8_t) ((divisor >> 8) & 0xFF));
    uint8_t gate = in(SPEAKER_CONTROL_PORT);
    if ((gate & 0x03) != 0x03)
        out(SPEAKER_CONTROL_PORT, gate | 0x03);
}

void speaker_stop(void) {
    out(SPEAKER_CONTROL_PORT, in(SPEAKER_CONTROL_PORT) & 0xFC);
}

void speaker_beep(uint32_t frequency_hz, uint32_t duration_ms) {
    speaker_tone(frequency_hz);
    timer_sleep_ms(duration_ms);
    speaker_stop();
}

void speaker_play_startup_chime(void) {
    // C5 - E5 - G5 - C6 arpeggio
    static const uint16_t notes[]     = {523, 659, 784, 1047};
    static const uint16_t durations[] = {90, 90, 90, 220};
    for (uint32_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
        speaker_beep(notes[i], durations[i]);
        timer_sleep_ms(15);
    }
}
