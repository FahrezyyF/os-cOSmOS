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

/**
 * Program PIT channel 0 as a periodic rate generator and unmask IRQ0.
 * Chapter 4 (scheduler) can hook the same IRQ for preemption.
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
