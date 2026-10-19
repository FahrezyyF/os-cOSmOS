#include "header/driver/timer.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/portio.h"

static volatile uint32_t timer_ticks = 0;
static uint32_t timer_ms_per_tick    = 1;

void timer_init(uint32_t frequency_hz) {
    if (frequency_hz == 0)
        frequency_hz = TIMER_FREQUENCY_HZ;
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;
    timer_ms_per_tick = 1000 / frequency_hz;
    if (timer_ms_per_tick == 0)
        timer_ms_per_tick = 1;

    out(PIT_COMMAND, 0x36); // Channel 0, lobyte/hibyte, mode 3 (square wave), binary
    out(PIT_CHANNEL0_DATA, (uint8_t) (divisor & 0xFF));
    out(PIT_CHANNEL0_DATA, (uint8_t) ((divisor >> 8) & 0xFF));
    pic_unmask_irq(IRQ_TIMER);
}

void timer_isr(void) {
    timer_ticks++;
    pic_ack(IRQ_TIMER);
}

uint32_t timer_get_ms(void) {
    return timer_ticks * timer_ms_per_tick;
}

void timer_sleep_ms(uint32_t ms) {
    uint32_t start = timer_get_ms();
    while (timer_get_ms() - start < ms)
        __asm__ volatile("hlt");
}
