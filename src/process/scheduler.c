#include "header/process/scheduler.h"
#include "header/process/pipe.h"
#include "header/memory/paging.h"
#include "header/driver/timer.h"
#include "header/driver/keyboard.h"
#include "header/driver/serial.h"
#include "header/kernel/statusbar.h"
#include "header/kernel/user.h"
#include "header/graphics/compositor.h"
#include "header/net/net.h"
#include "header/stdlib/string.h"

static bool                 active             = false;
static volatile bool        resched_requested  = false;
static uint32_t             slice_left_ms      = 0;
static uint32_t             rr_cursor          = 0;    // _process_list index that ran last
static struct SchedulerStats stats;

static bool from_user_mode(const struct InterruptFrame *frame) {
    return (frame->int_stack.cs & 0x3) == 0x3;
}

uint32_t scheduler_slice_ms(uint8_t priority) {
    switch (priority) {
        case PROCESS_PRIORITY_LOW:  return SCHEDULER_SLICE_LOW_MS;
        case PROCESS_PRIORITY_HIGH: return SCHEDULER_SLICE_HIGH_MS;
        default:                    return SCHEDULER_SLICE_NORMAL_MS;
    }
}

bool scheduler_is_active(void) {
    return active;
}

void scheduler_request_resched(void) {
    resched_requested = true;
}

void scheduler_get_stats(struct SchedulerStats *out) {
    *out = stats;
    out->active = active;
}

void scheduler_init(void) {
    // IRQ0 already ticks at PIT_TIMER_FREQUENCY (timer.c, activate_timer_interrupt()); from
    // now on every tick also drives the scheduler
    memset(&stats, 0, sizeof(stats));
    rr_cursor         = PROCESS_COUNT_MAX - 1; // the first pick starts at slot 0 (init)
    slice_left_ms     = 0;
    resched_requested = false;
    active            = true;
    serial_printf("[sched] round robin, slices %u/%u/%u ms (low/normal/high), timer %u Hz\n",
                  SCHEDULER_SLICE_LOW_MS, SCHEDULER_SLICE_NORMAL_MS, SCHEDULER_SLICE_HIGH_MS, TIMER_FREQUENCY_HZ);
}

void scheduler_context_from_frame(struct Context *ctx, const struct InterruptFrame *frame) {
    ctx->cpu    = frame->cpu; // intsetup.s already replaced cpu.stack.esp with the user esp
    ctx->eip    = frame->int_stack.eip;
    ctx->eflags = frame->int_stack.eflags;
    ctx->page_directory_virtual_addr = paging_get_current_page_directory_addr();
}

void scheduler_save_context_to_current_running_pcb(struct Context ctx) {
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    if (pcb != NULL)
        pcb->context = ctx;
}

/* ================================ WAKE-UP / REAPING ================================ */

static bool wait_satisfied(struct ProcessControlBlock *pcb, uint32_t now) {
    switch (pcb->metadata.wait_reason) {
        case PROCESS_WAIT_SLEEP:
            return (int32_t) (now - pcb->metadata.wake_time_ms) >= 0;
        case PROCESS_WAIT_KEY:
            return keyboard_has_input() && process_owns_keyboard(pcb);
        case PROCESS_WAIT_CHILD:
            return pcb->metadata.wait_done;
        case PROCESS_WAIT_PIPE_READ: {
            struct ProcessFile *f = process_fd_get(pcb, (int32_t) pcb->metadata.wait_arg);
            return f == NULL || f->type != PROCESS_FD_PIPE_READ || pipe_readable(f->pipe);
        }
        case PROCESS_WAIT_PIPE_WRITE: {
            struct ProcessFile *f = process_fd_get(pcb, (int32_t) pcb->metadata.wait_arg);
            return f == NULL || f->type != PROCESS_FD_PIPE_WRITE || pipe_writable(f->pipe);
        }
        case PROCESS_WAIT_WINDOW:
            return compositor_has_event(pcb->metadata.pid);
        case PROCESS_WAIT_NET:
            return net_has_datagram(pcb->metadata.pid, pcb->metadata.wait_arg) ||
                   (int32_t) (now - pcb->metadata.wake_time_ms) >= 0;
        default:
            return false; // PROCESS_WAIT_NONE: a PCB that is still being built
    }
}

static void wake_blocked(void) {
    uint32_t now = timer_get_ms();
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        struct ProcessControlBlock *pcb = &_process_list[i];
        if (pcb->metadata.state == PROCESS_STATE_BLOCKED && wait_satisfied(pcb, now)) {
            pcb->metadata.state       = PROCESS_STATE_READY;
            pcb->metadata.wait_reason = PROCESS_WAIT_NONE;
        }
    }
}

// Kills requested from interrupt context (Ctrl+C) are carried out here, at a safe point
static void reap_pending_kills(void) {
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        struct ProcessControlBlock *pcb = &_process_list[i];
        if (pcb->metadata.state != PROCESS_STATE_INACTIVE && pcb->metadata.state != PROCESS_STATE_RUNNING &&
            pcb->metadata.kill_pending && !(pcb->metadata.state == PROCESS_STATE_BLOCKED &&
                                            pcb->metadata.wait_reason == PROCESS_WAIT_NONE))
            process_terminate(pcb->metadata.pid, PROCESS_EXIT_KILLED);
    }
}

static bool other_ready_exists(const struct ProcessControlBlock *self) {
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++)
        if (&_process_list[i] != self && _process_list[i].metadata.state == PROCESS_STATE_READY)
            return true;
    return false;
}

/* ================================ ALGORITHM: ROUND ROBIN =========================== */

static struct ProcessControlBlock *pick_next(void) {
    for (uint32_t k = 1; k <= PROCESS_COUNT_MAX; k++) {
        uint32_t i = (rr_cursor + k) % PROCESS_COUNT_MAX;
        if (_process_list[i].metadata.state == PROCESS_STATE_READY) {
            rr_cursor = i;
            return &_process_list[i];
        }
    }
    return NULL;
}

void scheduler_switch_to_next_process(void) {
    __asm__ volatile("cli");
    struct ProcessControlBlock *next;
    for (;;) {
        reap_pending_kills();
        user_init_watchdog(); // (re)start the shell when init is gone
        wake_blocked();
        next = pick_next();
        if (next != NULL)
            break;
        // Nothing to run: idle in the kernel page directory until an interrupt makes a
        // process ready (timer for sleepers, keyboard for readers, ...)
        if (paging_get_current_page_directory_addr() != &_paging_kernel_page_directory)
            paging_use_page_directory(&_paging_kernel_page_directory);
        __asm__ volatile("sti");
        kernel_idle();
        __asm__ volatile("hlt");
        __asm__ volatile("cli");
    }
    next->metadata.state = PROCESS_STATE_RUNNING;
    next->metadata.switches++;
    stats.context_switches++;
    slice_left_ms     = scheduler_slice_ms(next->metadata.priority);
    resched_requested = false;

    // Virtual address space & process (book 4.2.2.2), then the CPU registers (assembly)
    paging_use_page_directory(next->context.page_directory_virtual_addr);
    process_context_switch(next->context);
}

/* ================================ HOOKS ============================================ */

__attribute__((noreturn)) static void preempt_current(const struct InterruptFrame *frame, struct ProcessControlBlock *pcb) {
    __asm__ volatile("cli");
    struct Context ctx;
    scheduler_context_from_frame(&ctx, frame);
    scheduler_save_context_to_current_running_pcb(ctx);
    pcb->metadata.state = PROCESS_STATE_READY;
    scheduler_switch_to_next_process();
}

void scheduler_on_timer(const struct InterruptFrame *frame) {
    if (!active)
        return;
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    if (pcb != NULL) {
        pcb->metadata.cpu_ms++;
        stats.busy_ms++;
    } else {
        stats.idle_ms++;
    }
    if (!from_user_mode(frame) || pcb == NULL)
        return; // the kernel (a system call, the idle loop) is never preempted
    kernel_idle(); // safe point: status bar, mouse pointer, window manager
    if (slice_left_ms > 0)
        slice_left_ms--;
    if (slice_left_ms == 0) {
        stats.preemptions++;
        resched_requested = true;
    }
}

void scheduler_on_return_to_user(const struct InterruptFrame *frame) {
    if (!active || !from_user_mode(frame))
        return;
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    if (pcb == NULL)
        return;
    if (pcb->metadata.kill_pending) {
        __asm__ volatile("cli");
        process_terminate(pcb->metadata.pid, PROCESS_EXIT_KILLED);
        scheduler_switch_to_next_process();
    }
    if (!resched_requested)
        return;
    reap_pending_kills();
    wake_blocked();
    if (other_ready_exists(pcb)) {
        preempt_current(frame, pcb);
    }
    // Alone: keep running with a fresh slice
    resched_requested = false;
    slice_left_ms     = scheduler_slice_ms(pcb->metadata.priority);
}

void scheduler_block_current(const struct InterruptFrame *frame, PROCESS_WAIT reason, uint32_t arg, bool restart) {
    __asm__ volatile("cli");
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    if (pcb == NULL)
        scheduler_switch_to_next_process();
    struct Context ctx;
    scheduler_context_from_frame(&ctx, frame);
    if (restart)
        ctx.eip -= 2; // back onto `int 0x30` (CD 30): the call runs again after the wake-up
    scheduler_save_context_to_current_running_pcb(ctx);
    pcb->metadata.wait_reason = reason;
    pcb->metadata.wait_arg    = arg;
    pcb->metadata.state       = PROCESS_STATE_BLOCKED;
    stats.blocks++;
    scheduler_switch_to_next_process();
}

void scheduler_yield_current(const struct InterruptFrame *frame) {
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    if (pcb == NULL)
        scheduler_switch_to_next_process();
    preempt_current(frame, pcb);
}
