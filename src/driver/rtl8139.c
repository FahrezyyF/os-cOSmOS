#include "header/net/net.h"
#include "header/driver/pci.h"
#include "header/driver/serial.h"
#include "header/driver/timer.h"
#include "header/cpu/portio.h"
#include "header/stdlib/string.h"
#include "header/memory/paging.h"

/**
 * Realtek RTL8139 driver (QEMU: -device rtl8139). The card writes received frames into
 * one physically contiguous ring buffer and sends from four transmit buffers; both live
 * in the kernel image (.bss), whose physical address is virtual - 0xC0000000.
 */

#define RTL_VENDOR        0x10EC
#define RTL_DEVICE        0x8139

#define REG_MAC           0x00
#define REG_TSD0          0x10 // transmit status of descriptor 0..3 (+4 each)
#define REG_TSAD0         0x20 // transmit start address 0..3
#define REG_RBSTART       0x30
#define REG_CMD           0x37
#define REG_CAPR          0x38
#define REG_IMR           0x3C
#define REG_ISR           0x3E
#define REG_TCR           0x40
#define REG_RCR           0x44
#define REG_CONFIG1       0x52

#define CMD_RESET         0x10
#define CMD_RX_ENABLE     0x08
#define CMD_TX_ENABLE     0x04
#define CMD_RX_BUF_EMPTY  0x01
#define INT_ROK           0x0001
#define INT_RER           0x0002
#define INT_TOK           0x0004
#define INT_TER           0x0008
#define INT_RX_OVERFLOW   0x0010
#define INT_RX_FIFO_OVER  0x0040
#define RCR_ACCEPT_ALL_PHYS 0x01
#define RCR_ACCEPT_MYMAC    0x02
#define RCR_ACCEPT_MULTI    0x04
#define RCR_ACCEPT_BCAST    0x08
#define RCR_WRAP            0x80
#define TSD_OWN             0x2000

#define RX_RING_SIZE      8192
#define RX_BUFFER_SIZE    (RX_RING_SIZE + 16 + 1536)
#define TX_BUFFER_SIZE    1792

static __attribute__((aligned(4096))) uint8_t rx_buffer[RX_BUFFER_SIZE];
static __attribute__((aligned(4096))) uint8_t tx_buffers[4][TX_BUFFER_SIZE];

static bool     present   = false;
static uint16_t io_base   = 0;
static uint8_t  irq_line  = 0;
static uint8_t  mac[6];
static uint32_t rx_offset = 0;
static uint8_t  tx_next   = 0;

static uint32_t physical(const void *kernel_virtual) {
    return (uint32_t) kernel_virtual - KERNEL_VIRTUAL_ADDRESS_BASE;
}

bool rtl8139_present(void) {
    return present;
}

uint8_t rtl8139_irq(void) {
    return irq_line;
}

void rtl8139_get_mac(uint8_t out[6]) {
    memcpy(out, mac, 6);
}

bool rtl8139_init(void) {
    struct PCIAddress dev;
    if (!pci_find_device(RTL_VENDOR, RTL_DEVICE, &dev))
        return false;
    uint32_t bar0 = pci_config_read32(dev, 0x10);
    if (!(bar0 & 0x1)) // must be an I/O space BAR
        return false;
    io_base  = (uint16_t) (bar0 & 0xFFFC);
    irq_line = (uint8_t) (pci_config_read32(dev, 0x3C) & 0xFF);
    // PCI command register: I/O space + bus mastering (the card DMAs into our buffers)
    uint32_t command = pci_config_read32(dev, 0x04);
    pci_config_write32(dev, 0x04, command | 0x5);

    out(io_base + REG_CONFIG1, 0x00); // power on
    out(io_base + REG_CMD, CMD_RESET);
    uint32_t start = timer_get_ms();
    while ((in(io_base + REG_CMD) & CMD_RESET) && timer_get_ms() - start < 100) {
    }
    for (int i = 0; i < 6; i++)
        mac[i] = in(io_base + REG_MAC + i);

    memset(rx_buffer, 0, sizeof(rx_buffer));
    out32(io_base + REG_RBSTART, physical(rx_buffer));
    out16(io_base + REG_IMR, INT_ROK | INT_TOK | INT_RER | INT_TER | INT_RX_OVERFLOW | INT_RX_FIFO_OVER);
    out32(io_base + REG_RCR, RCR_ACCEPT_MYMAC | RCR_ACCEPT_BCAST | RCR_ACCEPT_MULTI | RCR_WRAP);
    out32(io_base + REG_TCR, 0x03000000u); // standard interframe gap
    out(io_base + REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);
    rx_offset = 0;
    tx_next   = 0;
    present   = true;
    serial_printf("[net] RTL8139 at io 0x%x, IRQ %u, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", io_base, irq_line,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool rtl8139_send(const void *frame, uint32_t length) {
    if (!present || length > TX_BUFFER_SIZE)
        return false;
    if (length < 60)
        length = 60; // Ethernet minimum (the padding is zeroes below)
    uint16_t tsd = (uint16_t) (io_base + REG_TSD0 + tx_next * 4);
    // Wait until the card owns nothing in this descriptor (OWN = DMA finished)
    uint32_t start = timer_get_ms();
    while (!(in32(tsd) & TSD_OWN) && (in32(tsd) & 0x1FFF) != 0 && timer_get_ms() - start < 50) {
    }
    memset(tx_buffers[tx_next], 0, 60);
    memcpy(tx_buffers[tx_next], frame, length < TX_BUFFER_SIZE ? length : TX_BUFFER_SIZE);
    out32((uint16_t) (io_base + REG_TSAD0 + tx_next * 4), physical(tx_buffers[tx_next]));
    out32(tsd, length & 0x1FFF); // clearing OWN starts the transmission
    tx_next = (uint8_t) ((tx_next + 1) % 4);
    return true;
}

void rtl8139_handle_interrupt(void) {
    uint16_t status = in16(io_base + REG_ISR);
    out16(io_base + REG_ISR, status); // acknowledge (write 1 to clear)
    if (status & (INT_ROK | INT_RX_OVERFLOW | INT_RX_FIFO_OVER)) {
        while (!(in(io_base + REG_CMD) & CMD_RX_BUF_EMPTY)) {
            uint8_t *packet = rx_buffer + rx_offset;
            uint16_t rx_status = (uint16_t) (packet[0] | packet[1] << 8);
            uint16_t length    = (uint16_t) (packet[2] | packet[3] << 8); // includes the 4-byte CRC
            if (!(rx_status & 0x1) || length < 4 || length > 1792) { // bad packet: reset the ring
                out(io_base + REG_CMD, CMD_TX_ENABLE);
                rx_offset = 0;
                out32(io_base + REG_RBSTART, physical(rx_buffer));
                out(io_base + REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);
                break;
            }
            net_receive_frame(packet + 4, length - 4u);
            rx_offset = (rx_offset + length + 4 + 3) & ~3u;
            if (rx_offset >= RX_RING_SIZE)
                rx_offset -= RX_RING_SIZE;
            out16(io_base + REG_CAPR, (uint16_t) (rx_offset - 16));
        }
    }
}
