#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/gdt.h"
#include "header/kernel-entrypoint.h"
#include "header/text/framebuffer.h"
#include "header/text/console.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/idt.h"
#include "header/driver/keyboard.h"
#include "header/driver/serial.h"
#include "header/driver/timer.h"
#include "header/driver/mouse.h"
#include "header/driver/speaker.h"
#include "header/filesystem/vfs.h"
#include "header/kernel/splash.h"
#include "header/kernel/monitor.h"

#ifdef COSMOS_FORCE_TEXT_MODE
#define PREFER_GRAPHICS false
#else
#define PREFER_GRAPHICS true
#endif

static void boot_log(const char *line) {
    serial_printf("[boot] %s\n", line);
    splash_step(line, 85);
}

void kernel_setup(void) {
    // Chapter 0 & 1: CPU tables and interrupts
    load_gdt(&_gdt_gdtr);
    pic_remap();
    initialize_idt();
    serial_init();
    serial_printf("[boot] cOSmOS v0.2.0 kernel_setup()\n");

    // Display: BGA 1024x768x32 graphics when available, VGA text mode otherwise
    bool graphics = framebuffer_init(PREFER_GRAPHICS);
    console_init();
    serial_printf("[boot] display: %s, %ux%u cells\n", graphics ? "BGA graphics" : "VGA text",
                  framebuffer_get_cols(), framebuffer_get_rows());

    timer_init(TIMER_FREQUENCY_HZ);
    activate_keyboard_interrupt();
    keyboard_state_activate();

    splash_begin();
    splash_step("GDT, IDT, PIC remap, PIT timer and keyboard ready", 20);

    uint16_t pointer_w, pointer_h;
    framebuffer_get_pointer_space(&pointer_w, &pointer_h);
    bool mouse = mouse_init(pointer_w, pointer_h);
    serial_printf("[boot] ps/2 mouse: %s\n", mouse ? (mouse_has_wheel() ? "wheel mouse" : "present") : "absent");
    splash_step(mouse ? "PS/2 mouse detected (IRQ12)" : "No PS/2 mouse", 40);

    // Chapter 2: disks and file systems
    splash_step("Probing ATA disks...", 60);
    vfs_mount_all(boot_log);
    splash_step("File systems mounted - welcome aboard!", 100);
    speaker_play_startup_chime();
    splash_end(1200);

    monitor_run();
}
