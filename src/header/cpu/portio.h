#ifndef _PORTIO_H
#define _PORTIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** out:
 *  Sends the given data to the given I/O port
 *
 *  @param port The I/O port to send the data to
 *  @param data The data to send to the I/O port
 */
void out(uint16_t port, uint8_t data);

/** in:
 *  Read data from the given I/O port
 *
 *  @param port The I/O port to request the data
 *  @return Recieved data from the corresponding I/O port
 */
uint8_t in(uint16_t port);

/**
 * 16-bit port output (outw), used by the ATA PIO data port and power management ports
 *
 * @param port The I/O port to send the data to
 * @param data 16-bit data to send
 */
void out16(uint16_t port, uint16_t data);

/**
 * 16-bit port input (inw), used by the ATA PIO data port
 *
 * @param port The I/O port to read from
 * @return 16-bit data received from the port
 */
uint16_t in16(uint16_t port);

/**
 * 32-bit port output (outl), used by the PCI configuration space mechanism
 *
 * @param port The I/O port to send the data to
 * @param data 32-bit data to send
 */
void out32(uint16_t port, uint32_t data);

/**
 * 32-bit port input (inl), used by the PCI configuration space mechanism
 *
 * @param port The I/O port to read from
 * @return 32-bit data received from the port
 */
uint32_t in32(uint16_t port);

#endif
