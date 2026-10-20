#ifndef _POWER_H
#define _POWER_H

/**
 * Power off the virtual machine. Tries the ACPI PM1a control ports used by
 * QEMU (0x604), Bochs / old QEMU (0xB004) and VirtualBox (0x4004).
 * Falls back to halting the CPU if none of them work.
 */
void power_shutdown(void) __attribute__((noreturn));

// Reset the machine through the 8042 keyboard controller (falls back to a triple fault)
void power_reboot(void) __attribute__((noreturn));

#endif
