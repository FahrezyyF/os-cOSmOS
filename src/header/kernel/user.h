#ifndef _USER_H
#define _USER_H

#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/interrupt.h"

/**
 * User program management for Chapter 3: the shell runs in ring 3 inside the kernel page
 * directory (Chapter 4 will give every process its own address space).
 *
 * User virtual memory:
 *   0x00000000 - program image (flat binary, <= 1 MiB)  \  demand paged,
 *     ...      - stack, grows down from 0x00400000       |  swappable
 *   0x00400000 - heap, grows up with sbrk()              /
 */

#define USER_SHELL_NAME "shell"

// Policy for the page fault handler: is this user address part of the program's memory?
bool user_address_is_valid(uint32_t address);

/**
 * Validate a pointer passed in by a system call (never trust ring 3 input)
 * @return true if [ptr, ptr + size) lies entirely inside valid user memory
 */
bool user_range_is_valid(const void *ptr, uint32_t size);

// Same for a null-terminated string of at most max_len bytes
bool user_string_is_valid(const char *str, uint32_t max_len);

/**
 * Move the program break (heap end) by `increment` bytes. Pages are mapped lazily by the
 * page fault handler; shrinking frees the pages above the new break.
 * @param old_break Receives the previous break
 * @return false if the break would leave [USER_HEAP_BASE, USER_HEAP_LIMIT)
 */
bool user_sbrk(int32_t increment, uint32_t *old_break);

uint32_t user_get_break(void);

/**
 * Free the old user space, load the shell from the root directory of the active file
 * system into 0x0 and enter ring 3 with iret. Never returns.
 */
void user_launch_shell(void) __attribute__((noreturn));

// SYS_EXIT: the program finished, start a fresh shell
void user_program_exit(int32_t code) __attribute__((noreturn));

/**
 * A CPU exception (or an unresolvable page fault) happened in user mode or on behalf of a
 * user system call: report it like a segmentation fault and restart the shell.
 */
void user_program_crash(struct InterruptFrame *frame, const char *reason) __attribute__((noreturn));

#endif
