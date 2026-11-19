#include "header/kernel/user.h"
#include "header/kernel/panic.h"
#include "header/kernel/statusbar.h"
#include "header/process/process.h"
#include "header/process/scheduler.h"
#include "header/memory/paging.h"
#include "header/memory/swap.h"
#include "header/filesystem/vfs.h"
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/driver/keyboard.h"
#include "header/driver/serial.h"
#include "header/driver/timer.h"
#include "header/driver/power.h"
#include "header/stdlib/string.h"

#define QUICK_CRASH_MS     1000 // a shell dying sooner than this after its start...
#define QUICK_CRASH_LIMIT  3    // ...this many times in a row waits for a key before retrying

static uint32_t init_start_ms   = 0;
static uint32_t quick_crashes   = 0;
static bool     waiting_for_key = false;

/* ================================ ADDRESS VALIDATION =============================== */

bool user_address_is_valid(uint32_t address) {
    return process_address_is_valid(process_get_by_current_address_space(), address);
}

bool user_range_is_valid(const void *ptr, uint32_t size) {
    struct ProcessControlBlock *pcb = process_get_by_current_address_space();
    if (pcb == NULL)
        return false;
    uint32_t start = (uint32_t) ptr;
    if (size == 0)
        return start < USER_SPACE_LIMIT;
    uint32_t end = start + size;
    if (end <= start || end > USER_SPACE_LIMIT) // also rejects wrap-around
        return false;
    // Every page of the range must belong to a region (regions may be adjacent)
    for (uint32_t a = start; a < end;) {
        const struct ProcessMemoryRegion *r = process_find_region(pcb, a);
        if (r == NULL)
            return false;
        a = r->end;
    }
    return true;
}

bool user_string_is_valid(const char *str, uint32_t max_len) {
    for (uint32_t i = 0; i <= max_len; i++) {
        if (!user_address_is_valid((uint32_t) str + i))
            return false;
        if (str[i] == '\0')
            return true;
    }
    return false;
}

/* ================================ INIT (the shell) ================================= */

static void wait_key_then(const char *what) {
    kprintf("%s", what);
    keyboard_state_activate();
    __asm__ volatile("sti");
    char c = 0;
    while (c == 0) {
        kernel_idle();
        get_keyboard_buffer(&c);
        if (c == 0)
            __asm__ volatile("hlt");
    }
}

static void shell_missing(int32_t code) __attribute__((noreturn));
static void shell_missing(int32_t code) {
    console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    if (!vfs_ready())
        kprintf("\ncOSmOS: no root file system is mounted, cannot start /%s.\n", USER_SHELL_NAME);
    else
        kprintf("\ncOSmOS: cannot start /%s from the %s root: %s.\n", USER_SHELL_NAME, vfs_name(),
                process_create_error_name(code));
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    kprintf("Insert it from the host with `make insert-shell` (`make run` does it too), then reboot.\n");
    serial_printf("[user] shell missing (code %d)\n", (int) code);
    wait_key_then("Press any key to reboot...");
    power_reboot();
}

void user_start_init(void) {
    // Shell request (book 4.1.4): flat binaries are loaded at buf = 0x0, at most 1 MiB;
    // cOSmOS builds the shell as a CEX executable, the loader picks the format itself
    struct EXT2DriverRequest request = {
        .buf          = (uint8_t*) 0,
        .name         = USER_SHELL_NAME,
        .parent_inode = vfs_root(),
        .buffer_size  = 0x100000,
        .name_len     = sizeof(USER_SHELL_NAME) - 1,
        .is_directory = false,
    };
    process_manager_state.init_pid = 0;
    int32_t code = vfs_ready() ? process_create_user_process(request) : PROCESS_CREATE_FAIL_FS_READ_FAILURE;
    if (code != PROCESS_CREATE_SUCCESS)
        shell_missing(code);
    init_start_ms = timer_get_ms();
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    framebuffer_set_cursor_visible(true);
    serial_printf("[user] init = pid %u (/%s)\n", process_manager_state.init_pid, USER_SHELL_NAME);
}

void user_init_watchdog(void) {
    if (process_manager_state.init_pid != 0 && process_get_by_pid(process_manager_state.init_pid) != NULL)
        return;
    if (waiting_for_key) {
        if (!keyboard_has_input())
            return;
        char c;
        get_keyboard_buffer(&c);
        waiting_for_key = false;
    } else if (timer_get_ms() - init_start_ms < QUICK_CRASH_MS) {
        if (++quick_crashes >= QUICK_CRASH_LIMIT) {
            quick_crashes   = 0;
            waiting_for_key = true;
            console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
            kprintf("The shell keeps ending right after its start. Press any key to retry...\n");
            console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
            return;
        }
    } else {
        quick_crashes = 0;
    }
    console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
    kprintf("[init: starting a new %s]\n", USER_SHELL_NAME);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    process_manager_state.init_respawns++;
    __asm__ volatile("sti"); // the loader reads the disk; let the timer keep ticking
    user_start_init();
    __asm__ volatile("cli");
}

/* ================================ CRASHES ========================================== */

void user_program_crash(struct InterruptFrame *frame, const char *reason) {
    __asm__ volatile("cli");
    uint32_t vector = frame->int_number;
    if (reason == NULL)
        reason = kernel_exception_name(vector);
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    const char *name = pcb != NULL ? pcb->metadata.name : "?";
    uint32_t    pid  = pcb != NULL ? pcb->metadata.pid : 0;
    uint8_t row, col;
    console_get_cursor(&row, &col);
    console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    if (col != 0)
        kprintf("\n");
    kprintf("*** %s (pid %u) crashed: %s (exception #%u, error 0x%x) at eip 0x%08x", name, pid, reason,
            vector, frame->int_stack.error_code, frame->int_stack.eip);
    if (vector == 14)
        kprintf(", address 0x%08x", read_cr2());
    kprintf("\n");
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    serial_printf("[user] crash of pid %u: %s, vector %u, eip 0x%08x, cr2 0x%08x\n", pid, reason, vector,
                  frame->int_stack.eip, read_cr2());
    if (pcb != NULL)
        process_terminate(pid, PROCESS_EXIT_CRASHED);
    scheduler_switch_to_next_process();
}
