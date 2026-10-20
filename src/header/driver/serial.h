#ifndef _SERIAL_H
#define _SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#define SERIAL_COM1_PORT 0x3F8

/**
 * Initialize COM1 UART (115200 baud, 8N1, FIFO enabled, interrupts off).
 * QEMU forwards COM1 to `-serial file:bin/serial.log`, which makes it a cheap
 * kernel debug log that keeps working even when the screen is broken.
 *
 * @return true if the UART passed the loopback self-test
 */
bool serial_init(void);

// Write one character to COM1 (no-op if the port is absent). '\n' is sent as "\r\n"
void serial_putc(char c);

// Write a null-terminated string to COM1
void serial_puts(const char *s);

// printf-style logging to COM1
void serial_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
