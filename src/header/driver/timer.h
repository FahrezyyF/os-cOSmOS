#ifndef _TIMER_H
#define _TIMER_H

#include <stdint.h>
#include <stdbool.h>

// 8253/8254 Programmable Interval Timer
#define PIT_BASE_FREQUENCY 1193182u
#define PIT_CHANNEL0_DATA  0x40
#define PIT_CHANNEL2_DATA  0x42
#define PIT_COMMAND        0x43

// Kernel tick rate, 1 tick = 1 ms
#define TIMER_FREQUENCY_HZ 1000u

// Book 4.2.1.1 constants
#define PIT_MAX_FREQUENCY   1193182
#define PIT_TIMER_FREQUENCY 1000
#define PIT_TIMER_COUNTER   (PIT_MAX_FREQUENCY / PIT_TIMER_FREQUENCY)

#define PIT_COMMAND_REGISTER_PIO          0x43
#define PIT_COMMAND_VALUE_BINARY_MODE     0b0
#define PIT_COMMAND_VALUE_OPR_SQUARE_WAVE (0b011 << 1)
#define PIT_COMMAND_VALUE_ACC_LOHIBYTE    (0b11  << 4)
#define PIT_COMMAND_VALUE_CHANNEL         (0b00  << 6)
#define PIT_COMMAND_VALUE (PIT_COMMAND_VALUE_BINARY_MODE | PIT_COMMAND_VALUE_OPR_SQUARE_WAVE | PIT_COMMAND_VALUE_ACC_LOHIBYTE | PIT_COMMAND_VALUE_CHANNEL)

#define PIT_CHANNEL_0_DATA_PIO 0x40

/**
 * Book 4.2.1.1: program the PIT to fire IRQ0 periodically (PIT_TIMER_FREQUENCY) and unmask
 * IRQ0 on the master PIC. Leaves interrupts disabled (cli), like the book's version.
 */
void activate_timer_interrupt(void);

/**
 * Start the kernel tick: activate_timer_interrupt() + sti. IRQ0 also drives the
 * scheduler (process/scheduler.c).
 *
 * @param frequency_hz Tick frequency in Hz
 */
void timer_init(uint32_t frequency_hz);

// IRQ0 handler, called by main_interrupt_handler(). Increments ticks and ACKs the PIC
void timer_isr(void);

// Milliseconds since timer_init() (wraps after ~49 days)
uint32_t timer_get_ms(void);

/**
 * Sleep using hlt until at least `ms` milliseconds passed.
 * Requires interrupts enabled (sti already executed by initialize_idt()).
 */
void timer_sleep_ms(uint32_t ms);

#endif
