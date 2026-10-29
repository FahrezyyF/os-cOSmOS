#include "header/kernel/user.h"
#include "header/kernel-entrypoint.h"
#include "header/kernel/panic.h"
#include "header/kernel/statusbar.h"
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

static uint32_t user_break       = USER_HEAP_BASE;
static uint32_t launch_time_ms   = 0;
static uint32_t quick_crashes    = 0;

/* ================================ ADDRESS VALIDATION =============================== */

bool user_address_is_valid(uint32_t address) {
    // [0, 4 MiB): program image, bss and stack - [4 MiB, break): sbrk() heap
    return address < user_break;
}

bool user_range_is_valid(const void *ptr, uint32_t size) {
    uint32_t start = (uint32_t) ptr;
    if (size == 0)
        return start <= user_break;
    uint32_t end = start + size;
    return end > start && end <= user_break; // end > start also rejects wrap-around
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

/* ================================ HEAP (sbrk) ====================================== */

bool user_sbrk(int32_t increment, uint32_t *old_break) {
    uint32_t old = user_break;
    if (increment > 0) {
        if ((uint32_t) increment > USER_HEAP_LIMIT - old)
            return false;
        // Commit limit: free RAM + free swap must be able to back the new memory
        if (!paging_allocate_check((uint32_t) increment))
            return false;
        user_break = old + (uint32_t) increment;
    } else if (increment < 0) {
        uint32_t decrement = (uint32_t) (-(int64_t) increment);
        if (decrement > old - USER_HEAP_BASE)
            return false;
        user_break = old - decrement;
        // Give back every page that is now entirely above the break
        uint32_t first = (user_break + PAGE_FRAME_SIZE - 1) & ~(PAGE_FRAME_SIZE - 1);
        for (uint32_t page = first; page < old; page += PAGE_FRAME_SIZE)
            paging_free_user_page_frame(&_paging_kernel_page_directory, (void*) page);
    }
    *old_break = old;
    return true;
}

uint32_t user_get_break(void) {
    return user_break;
}

/* ================================ PROGRAM LIFETIME ================================= */

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

static void shell_missing(int8_t code) __attribute__((noreturn));
static void shell_missing(int8_t code) {
    console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    if (!vfs_ready())
        kprintf("\ncOSmOS: no root file system is mounted, cannot start /%s.\n", USER_SHELL_NAME);
    else
        kprintf("\ncOSmOS: cannot load /%s from the %s root (read error %d).\n", USER_SHELL_NAME, vfs_name(), code);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    kprintf("Insert it from the host with `make insert-shell` (`make run` does it too), then reboot.\n");
    serial_printf("[user] shell missing (code %d)\n", code);
    wait_key_then("Press any key to reboot...");
    power_reboot();
}

void user_launch_shell(void) {
    // Start from an empty user space: every page / swap slot of the previous program is freed
    paging_free_user_space(&_paging_kernel_page_directory);
    user_break = USER_HEAP_BASE;

    // Allocate first virtual page (book 3.3.5); everything else is mapped on demand by the
    // page fault handler while read() fills the image
    paging_allocate_user_page_frame(&_paging_kernel_page_directory, (void*) USER_PROGRAM_BASE);

    // Write shell into memory
    struct EXT2DriverRequest request = {
        .buf          = (uint8_t*) USER_PROGRAM_BASE,
        .name         = USER_SHELL_NAME,
        .name_len     = sizeof(USER_SHELL_NAME) - 1,
        .parent_inode = vfs_root(),
        .buffer_size  = USER_PROGRAM_MAX_SIZE,
        .is_directory = false,
    };
    int8_t code = vfs_ready() ? vfs_read(request) : -1;
    if (code != 0)
        shell_missing(code);

    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    framebuffer_set_cursor_visible(true);
    launch_time_ms = timer_get_ms();
    serial_printf("[user] launching /%s in ring 3 (%u resident pages)\n", USER_SHELL_NAME,
                  page_manager_state.user_page_count);

    // TSS esp0 was set once by kernel_setup(); every ring 3 -> ring 0 switch starts there, so
    // jumping from inside a handler (exit, crash) does not leak the kernel stack
    kernel_execute_user_program((void*) USER_PROGRAM_BASE);
}

void user_program_exit(int32_t code) {
    serial_printf("[user] /%s exited with code %d\n", USER_SHELL_NAME, (int) code);
    console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
    kprintf("[%s exited with code %d, starting a new one]\n", USER_SHELL_NAME, (int) code);
    quick_crashes = 0;
    user_launch_shell();
}

void user_program_crash(struct InterruptFrame *frame, const char *reason) {
    uint32_t vector = frame->int_number;
    if (reason == NULL)
        reason = kernel_exception_name(vector);
    uint8_t row, col;
    console_get_cursor(&row, &col);
    console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    if (col != 0)
        kprintf("\n");
    kprintf("*** %s crashed: %s (exception #%u, error 0x%x) at eip 0x%08x", USER_SHELL_NAME, reason,
            vector, frame->int_stack.error_code, frame->int_stack.eip);
    if (vector == 14)
        kprintf(", address 0x%08x", read_cr2());
    kprintf("\n");
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    serial_printf("[user] crash: %s, vector %u, eip 0x%08x, cr2 0x%08x\n", reason, vector,
                  frame->int_stack.eip, read_cr2());

    if (timer_get_ms() - launch_time_ms < QUICK_CRASH_MS) {
        if (++quick_crashes >= QUICK_CRASH_LIMIT) {
            quick_crashes = 0;
            wait_key_then("The shell keeps crashing right after start. Press any key to retry...\n");
        }
    } else {
        quick_crashes = 0;
    }
    user_launch_shell();
}
