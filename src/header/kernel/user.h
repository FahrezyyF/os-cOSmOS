#ifndef _USER_H
#define _USER_H

#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/interrupt.h"

/**
 * User program support on top of the process layer (header/process/process.h):
 * pointer validation for system calls and the page fault handler, crashes, and the init
 * process (the shell), which is restarted whenever it ends.
 */

#define USER_SHELL_NAME "shell"

// Policy for the page fault handler: is this address part of the address space in CR3?
bool user_address_is_valid(uint32_t address);

/**
 * Validate a pointer passed in by a system call (never trust ring 3 input)
 * @return true if [ptr, ptr + size) lies entirely inside the current process' memory
 */
bool user_range_is_valid(const void *ptr, uint32_t size);

// Same for a null-terminated string of at most max_len bytes
bool user_string_is_valid(const char *str, uint32_t max_len);

/**
 * Start the shell as init (PID 1 at boot). Prints a message and reboots after a key
 * press when /shell cannot be loaded.
 */
void user_start_init(void);

/**
 * Called by the scheduler loop: when init is gone (exit / crash) a new shell is started.
 * A shell that keeps crashing right after its start waits for a key first.
 */
void user_init_watchdog(void);

/**
 * A CPU exception (or an unresolvable page fault) happened in user mode or on behalf of a
 * user system call: report it like a segmentation fault and end that process. Never returns.
 */
void user_program_crash(struct InterruptFrame *frame, const char *reason) __attribute__((noreturn));

#endif
