#ifndef _SCHEDULER_H
#define _SCHEDULER_H

#include "header/process/process.h"

/**
 * Preemptive scheduler (book 4.2): IRQ0 fires every millisecond (PIT 1000 Hz); when the
 * running process used up its time slice the scheduler saves its context and switches to
 * the next READY process in round-robin order. The time slice depends on the priority
 * (`nice`): low 5 ms, normal 10 ms, high 20 ms.
 *
 * There is one kernel stack (book 4.1.3: no reentrancy), so the kernel itself is never
 * preempted: contexts are only saved when an interrupt arrives from ring 3, and a system
 * call that has to wait (sleep, getchar, wait, pipe I/O) blocks the process by saving its
 * USER context, optionally rewound by 2 bytes so the `int 0x30` runs again when the process
 * is woken ("restartable system call"). Every switch is an iret, abandoning the kernel stack.
 */

#define SCHEDULER_SLICE_LOW_MS     5
#define SCHEDULER_SLICE_NORMAL_MS  10
#define SCHEDULER_SLICE_HIGH_MS    20

#define PROCESS_PRIORITY_LOW       0
#define PROCESS_PRIORITY_NORMAL    1
#define PROCESS_PRIORITY_HIGH      2

/**
 * Read all general purpose register values and set control register.
 * Resume the execution flow back to ctx.eip and ctx.eflags
 *
 * @note          Implemented in assembly
 * @param context Target context to switch into
 */
__attribute__((noreturn)) extern void process_context_switch(struct Context ctx);



/* --- Scheduler --- */
/**
 * Initialize scheduler before executing init process
 */
void scheduler_init(void);

/**
 * Save context to current running process
 *
 * @param ctx Context to save to current running process control block
 */
void scheduler_save_context_to_current_running_pcb(struct Context ctx);

/**
 * Trigger the scheduler algorithm and context switch to new process
 */
__attribute__((noreturn)) void scheduler_switch_to_next_process(void);

/* --- cOSmOS extensions --- */

struct SchedulerStats {
    uint32_t context_switches;
    uint32_t idle_ms;
    uint32_t busy_ms;
    uint32_t preemptions;   // slice used up
    uint32_t blocks;        // system calls that waited
    bool     active;
};

// Build a context from the user frame of an interrupt / system call
void scheduler_context_from_frame(struct Context *ctx, const struct InterruptFrame *frame);

/**
 * IRQ0 hook: account CPU time and preempt the running process when its slice is over.
 * Only acts when the interrupt came from ring 3. May not return.
 */
void scheduler_on_timer(const struct InterruptFrame *frame);

/**
 * Preemption point at the end of every interrupt / system call from ring 3: destroys the
 * current process if a kill is pending, or yields when another process should run now.
 */
void scheduler_on_return_to_user(const struct InterruptFrame *frame);

/**
 * Block the current process inside a system call. `restart` rewinds eip to the `int 0x30`
 * so the call is executed again after the wake-up. Never returns.
 */
__attribute__((noreturn)) void scheduler_block_current(const struct InterruptFrame *frame, PROCESS_WAIT reason,
                                                       uint32_t arg, bool restart);

// SYS_YIELD: give the CPU away for the rest of the slice
__attribute__((noreturn)) void scheduler_yield_current(const struct InterruptFrame *frame);

// Ask for a reschedule at the next preemption point (e.g. a blocked process was woken)
void scheduler_request_resched(void);

void scheduler_get_stats(struct SchedulerStats *stats);
uint32_t scheduler_slice_ms(uint8_t priority);

// True once the init process runs (before that, IRQ0 only counts ticks)
bool scheduler_is_active(void);

#endif
