#include "header/kernel/panic.h"
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/driver/serial.h"
#include "header/stdlib/string.h"

static const char *exception_names[32] = {
    "Division Error",            "Debug",                       "Non-Maskable Interrupt", "Breakpoint",
    "Overflow",                  "Bound Range Exceeded",        "Invalid Opcode",         "Device Not Available",
    "Double Fault",              "Coprocessor Segment Overrun", "Invalid TSS",            "Segment Not Present",
    "Stack-Segment Fault",       "General Protection Fault",    "Page Fault",             "Reserved",
    "x87 Floating-Point Error",  "Alignment Check",             "Machine Check",          "SIMD Floating-Point",
    "Virtualization Exception",  "Control Protection",          "Reserved",               "Reserved",
    "Reserved",                  "Reserved",                    "Reserved",               "Reserved",
    "Hypervisor Injection",      "VMM Communication",           "Security Exception",     "Reserved",
};

const char *kernel_exception_name(uint32_t vector) {
    return vector < 32 ? exception_names[vector] : "Unknown";
}

static void panic_halt(void) __attribute__((noreturn));
static void panic_halt(void) {
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void panic_screen_begin(const char *title) {
    __asm__ volatile("cli");
    framebuffer_pointer_set_visible(false);
    console_init();
    console_set_color(COLOR_WHITE, COLOR_BLUE);
    console_clear();
    framebuffer_set_cursor_visible(false);
    kprintf("\n  :(  cOSmOS ran into a problem and has been halted.\n\n");
    console_set_color(COLOR_YELLOW, COLOR_BLUE);
    kprintf("  %s\n\n", title);
    console_set_color(COLOR_WHITE, COLOR_BLUE);
}

void kernel_panic(const char *message) {
    panic_screen_begin("KERNEL_PANIC");
    kprintf("  Reason : %s\n", message);
    kprintf("\n  The system is halted. Restart QEMU to continue.\n");
    panic_halt();
}

void kernel_panic_exception(struct InterruptFrame *frame) {
    uint32_t vector = frame->int_number;
    const char *name = kernel_exception_name(vector);
    panic_screen_begin("UNHANDLED_CPU_EXCEPTION");

    kprintf("  Exception  : #%u %s\n", vector, name);
    kprintf("  Error code : 0x%08x\n", frame->int_stack.error_code);
    kprintf("  EIP        : 0x%08x   CS: 0x%04x   EFLAGS: 0x%08x\n",
            frame->int_stack.eip, frame->int_stack.cs, frame->int_stack.eflags);
    if (vector == 14) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        uint32_t err = frame->int_stack.error_code;
        kprintf("  CR2        : 0x%08x (%s, %s, %s mode)\n", cr2,
                (err & 0x1) ? "protection violation" : "page not present",
                (err & 0x2) ? "write" : "read",
                (err & 0x4) ? "user" : "kernel");
    }
    kprintf("\n  EAX=%08x  EBX=%08x  ECX=%08x  EDX=%08x\n",
            frame->cpu.general.eax, frame->cpu.general.ebx, frame->cpu.general.ecx, frame->cpu.general.edx);
    kprintf("  ESI=%08x  EDI=%08x  EBP=%08x  ESP=%08x\n",
            frame->cpu.index.esi, frame->cpu.index.edi, frame->cpu.stack.ebp, frame->cpu.stack.esp);
    kprintf("  DS=%04x  ES=%04x  FS=%04x  GS=%04x\n",
            frame->cpu.segment.ds, frame->cpu.segment.es, frame->cpu.segment.fs, frame->cpu.segment.gs);
    kprintf("\n  Tip: `make debug`, then in gdb: `info symbol 0x%08x`\n", frame->int_stack.eip);
    kprintf("\n  The system is halted. Restart QEMU to continue.\n");
    panic_halt();
}
