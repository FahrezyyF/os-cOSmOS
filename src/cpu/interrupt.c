#include "header/cpu/interrupt.h"
#include "header/cpu/portio.h"
#include "header/driver/keyboard.h"
#include "header/driver/mouse.h"
#include "header/driver/timer.h"
#include "header/cpu/gdt.h"
#include "header/kernel/panic.h"
#include "header/kernel/user.h"
#include "header/memory/paging.h"
#include "header/memory/swap.h"

struct TSSEntry _interrupt_tss_entry = {
    .ss0  = GDT_KERNEL_DATA_SEGMENT_SELECTOR,
};

void set_tss_kernel_current_stack(void) {
    uint32_t stack_ptr;
    // Reading base stack frame instead esp
    __asm__ volatile ("mov %%ebp, %0": "=r"(stack_ptr) : /* <Empty> */);
    // Add 8 because 4 for ret address and other 4 is for stack_ptr variable
    _interrupt_tss_entry.esp0 = stack_ptr + 8;
}

void io_wait(void) {
    out(0x80, 0);
}

void pic_ack(uint8_t irq) {
    if (irq >= 8) out(PIC2_COMMAND, PIC_ACK);
    out(PIC1_COMMAND, PIC_ACK);
}

void pic_remap(void) {
    // Starts the initialization sequence in cascade mode
    out(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    out(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    out(PIC1_DATA, PIC1_OFFSET); // ICW2: Master PIC vector offset
    io_wait();
    out(PIC2_DATA, PIC2_OFFSET); // ICW2: Slave PIC vector offset
    io_wait();
    out(PIC1_DATA, 0b0100); // ICW3: tell Master PIC, slave PIC at IRQ2 (0000 0100)
    io_wait();
    out(PIC2_DATA, 0b0010); // ICW3: tell Slave PIC its cascade identity (0000 0010)
    io_wait();

    out(PIC1_DATA, ICW4_8086);
    io_wait();
    out(PIC2_DATA, ICW4_8086);
    io_wait();

    // Disable all interrupts
    out(PIC1_DATA, PIC_DISABLE_ALL_MASK);
    out(PIC2_DATA, PIC_DISABLE_ALL_MASK);
}

#define EXCEPTION_PAGE_FAULT 14

static bool from_user_mode(const struct InterruptFrame *frame) {
    return (frame->int_stack.cs & 0x3) == 0x3;
}

static void handle_exception(struct InterruptFrame *frame) {
    uint32_t vector = frame->int_number;
    if (vector == EXCEPTION_PAGE_FAULT) {
        // Demand paging / swap in (book 3.2.1); returning re-executes the faulting instruction
        if (handle_page_fault(frame->int_stack.error_code))
            return;
        // A user program touched memory it does not own, or a system call was handed such a
        // pointer: kill the program, not the kernel
        if (from_user_mode(frame) || read_cr2() < USER_SPACE_LIMIT)
            user_program_crash(frame, swap_last_fault_was_oom() ? "Out of memory (RAM and swap are full)"
                                                                : "Segmentation fault");
        kernel_panic_exception(frame);
    }
    // Debug, breakpoint and overflow are traps that can safely resume (the book's `int $0x4` test)
    if (vector == 0x1 || vector == 0x3 || vector == 0x4)
        return;
    if (from_user_mode(frame))
        user_program_crash(frame, NULL);
    kernel_panic_exception(frame);
}

void main_interrupt_handler(struct InterruptFrame frame) {
    if (frame.int_number < PIC1_OFFSET) {
        handle_exception(&frame);
        return;
    }

    switch (frame.int_number) {
        case PIC1_OFFSET + IRQ_TIMER:
            timer_isr();
            break;
        case PIC1_OFFSET + IRQ_KEYBOARD:
            keyboard_isr();
            break;
        case PIC1_OFFSET + IRQ_MOUSE:
            mouse_isr();
            break;
        case 0x30:
            syscall(frame);
            break;
        default:
            // Spurious / unexpected IRQ: still ACK so the PIC does not stall
            if (frame.int_number >= PIC1_OFFSET && frame.int_number < PIC2_OFFSET + 8)
                pic_ack((uint8_t) (frame.int_number - PIC1_OFFSET));
            break;
    }
}

void activate_keyboard_interrupt(void) {
    out(PIC1_DATA, in(PIC1_DATA) & ~(1 << IRQ_KEYBOARD));
}

void pic_unmask_irq(uint8_t irq) {
    if (irq < 8) {
        out(PIC1_DATA, in(PIC1_DATA) & ~(1 << irq));
    } else {
        out(PIC2_DATA, in(PIC2_DATA) & ~(1 << (irq - 8)));
        out(PIC1_DATA, in(PIC1_DATA) & ~(1 << IRQ_CASCADE));
    }
}

void pic_mask_irq(uint8_t irq) {
    if (irq < 8)
        out(PIC1_DATA, in(PIC1_DATA) | (1 << irq));
    else
        out(PIC2_DATA, in(PIC2_DATA) | (1 << (irq - 8)));
}
