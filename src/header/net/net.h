#ifndef _NET_H
#define _NET_H

#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/interrupt.h"

/**
 * Network (bonus: hardware driver, UDP only): RTL8139 Fast Ethernet driver
 * (driver/rtl8139.c) and a small IPv4 stack (net/net.c): Ethernet, ARP (cache + replies),
 * IPv4, ICMP echo (ping replies and requests) and UDP sockets for user programs.
 *
 * Address configuration is static and matches QEMU's user-mode network ("slirp"):
 *   10.0.2.15/24, gateway 10.0.2.2 (also the host), DNS server 10.0.2.3.
 */

#define NET_MTU             1500
#define NET_UDP_PAYLOAD_MAX (NET_MTU - 20 - 8)
#define NET_SOCKET_MAX      8
#define NET_SOCKET_QUEUE    8

/* -- driver interface (driver/rtl8139.c) -- */
bool     rtl8139_init(void);
bool     rtl8139_present(void);
uint8_t  rtl8139_irq(void);
void     rtl8139_get_mac(uint8_t mac[6]);
bool     rtl8139_send(const void *frame, uint32_t length);
// IRQ handler: drains the receive ring, calls net_receive_frame() per packet
void     rtl8139_handle_interrupt(void);

/* -- stack -- */
// Bring the interface up (after the driver); false when there is no network card
bool net_init(void);
// Driver callback for every received Ethernet frame
void net_receive_frame(const uint8_t *frame, uint32_t length);

// Interrupt dispatch: true when `irq` belongs to the network card (handled + ACKed)
bool net_handle_irq(uint8_t irq);

bool net_has_datagram(uint32_t pid, uint32_t socket);
void net_on_process_exit(uint32_t pid);
void net_syscall(const struct InterruptFrame *frame, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

#endif
