#include "header/driver/timer.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/portio.h"

static volatile uint32_t timer_ticks = 0;
static uint32_t timer_ms_per_tick    = 1;

// Book 4.2.1.1, verbatim: PIT channel 0 in square wave mode at PIT_TIMER_FREQUENCY, IRQ0 unmasked
void activate_timer_interrupt(void) {
    __asm__ volatile("cli");
    // Setup how often PIT fire
    uint32_t pit_timer_counter_to_fire = PIT_TIMER_COUNTER;
    out(PIT_COMMAND_REGISTER_PIO, PIT_COMMAND_VALUE);
    out(PIT_CHANNEL_0_DATA_PIO, (uint8_t) (pit_timer_counter_to_fire & 0xFF));
    out(PIT_CHANNEL_0_DATA_PIO, (uint8_t) ((pit_timer_counter_to_fire >> 8) & 0xFF));

    // Activate the interrupt
    out(PIC1_DATA, in(PIC1_DATA) & ~(1 << IRQ_TIMER));
}

void timer_init(uint32_t frequency_hz) {
    (void) frequency_hz; // the kernel tick is fixed at PIT_TIMER_FREQUENCY (1 tick = 1 ms)
    timer_ms_per_tick = 1000 / PIT_TIMER_FREQUENCY;
    if (timer_ms_per_tick == 0)
        timer_ms_per_tick = 1;
    activate_timer_interrupt();
    __asm__ volatile("sti"); // activate_timer_interrupt() leaves interrupts off
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
