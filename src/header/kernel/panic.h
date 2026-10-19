#ifndef _PANIC_H
#define _PANIC_H

#include "header/cpu/interrupt.h"

/**
 * Stop the kernel with a full-screen error report (cOSmOS "blue screen").
 * Interrupts are disabled and the CPU halts forever; the report is also sent to COM1.
 *
 * @param message Human readable reason
 */
void kernel_panic(const char *message) __attribute__((noreturn));

/**
 * Panic caused by an unhandled CPU exception. Shows the exception name,
 * error code, faulting EIP/CS/EFLAGS, general registers and CR2 for page faults.
 *
 * @param frame Interrupt frame captured by the ISR stub
 */
void kernel_panic_exception(struct InterruptFrame *frame) __attribute__((noreturn));

#endif
