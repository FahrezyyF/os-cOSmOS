#include <stdarg.h>
#include "header/driver/serial.h"
#include "header/cpu/portio.h"
#include "header/stdlib/format.h"

#define UART_DATA          0 // DLAB=0: data, DLAB=1: divisor low
#define UART_INT_ENABLE    1 // DLAB=0: interrupt enable, DLAB=1: divisor high
#define UART_FIFO_CONTROL  2
#define UART_LINE_CONTROL  3
#define UART_MODEM_CONTROL 4
#define UART_LINE_STATUS   5
#define UART_LSR_THR_EMPTY 0x20

static bool serial_ready = false;

bool serial_init(void) {
    uint16_t base = SERIAL_COM1_PORT;
    out(base + UART_INT_ENABLE, 0x00);    // Polling only, no UART interrupts
    out(base + UART_LINE_CONTROL, 0x80);  // DLAB on
    out(base + UART_DATA, 0x01);          // Divisor 1 -> 115200 baud
    out(base + UART_INT_ENABLE, 0x00);
    out(base + UART_LINE_CONTROL, 0x03);  // 8 bits, no parity, 1 stop bit, DLAB off
    out(base + UART_FIFO_CONTROL, 0xC7);  // Enable & clear FIFO, 14-byte threshold
    out(base + UART_MODEM_CONTROL, 0x1E); // Loopback mode for the self-test

    out(base + UART_DATA, 0xAE);
    if (in(base + UART_DATA) != 0xAE) {
        serial_ready = false;
        return false;
    }

    out(base + UART_MODEM_CONTROL, 0x0F); // Normal mode: DTR, RTS, OUT1, OUT2
    serial_ready = true;
    return true;
}

static void serial_write_raw(char c) {
    // Bounded wait so a missing / stuck UART can never hang the kernel
    for (uint32_t spin = 0; spin < 100000; spin++) {
        if (in(SERIAL_COM1_PORT + UART_LINE_STATUS) & UART_LSR_THR_EMPTY) {
            out(SERIAL_COM1_PORT + UART_DATA, (uint8_t) c);
            return;
        }
    }
}

void serial_putc(char c) {
    if (!serial_ready)
        return;
    if (c == '\n')
        serial_write_raw('\r');
    serial_write_raw(c);
}

void serial_puts(const char *s) {
    while (*s != '\0')
        serial_putc(*s++);
}

void serial_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts(buf);
}
