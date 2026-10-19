#include <stdint.h>
#include "header/driver/power.h"
#include "header/cpu/portio.h"

void power_shutdown(void) {
    __asm__ volatile("cli");
    out16(0x604, 0x2000);  // QEMU (PIIX4 ACPI, -machine pc)
    out16(0xB004, 0x2000); // Bochs and QEMU < 2.0
    out16(0x4004, 0x3400); // VirtualBox
    for (;;)
        __asm__ volatile("hlt");
}

void power_reboot(void) {
    __asm__ volatile("cli");
    // Wait until the 8042 input buffer is empty, then pulse the CPU reset line
    for (uint32_t spin = 0; spin < 100000 && (in(0x64) & 0x02); spin++)
        ;
    out(0x64, 0xFE);

    // Fallback: load an empty IDT and trigger an interrupt -> triple fault -> reset
    struct {
        uint16_t size;
        uint32_t address;
    } __attribute__((packed)) empty_idt = {0, 0};
    __asm__ volatile("lidt %0; int $3" : : "m"(empty_idt));
    for (;;)
        __asm__ volatile("hlt");
}
