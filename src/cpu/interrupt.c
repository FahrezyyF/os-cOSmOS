#include "header/cpu/interrupt.h"
#include "header/cpu/portio.h"
#include "header/driver/keyboard.h"
#include "header/driver/mouse.h"
#include "header/driver/timer.h"
#include "header/kernel/panic.h"

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

void main_interrupt_handler(struct InterruptFrame frame) {
    if (frame.int_number < PIC1_OFFSET) {
        // CPU exception: debug, breakpoint, and overflow are traps that can safely
        // resume (e.g. the book's `int $0x4` test), every other exception is fatal.
        if (frame.int_number == 0x1 || frame.int_number == 0x3 || frame.int_number == 0x4)
            return;
        kernel_panic_exception(&frame);
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
