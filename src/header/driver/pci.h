#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>
#include <stdbool.h>

// PCI configuration space access mechanism #1
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_VENDOR_NONE    0xFFFF

/**
 * PCIAddress, location of one PCI function
 *
 * @param bus      Bus number (0-255)
 * @param slot     Device number (0-31)
 * @param function Function number (0-7)
 */
struct PCIAddress {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
};

/**
 * Read a 32-bit register from PCI configuration space
 *
 * @param addr   Device location
 * @param offset Register offset, must be 4-byte aligned
 * @return Register value, 0xFFFFFFFF if the device does not exist
 */
uint32_t pci_config_read32(struct PCIAddress addr, uint8_t offset);

/**
 * Brute-force scan every bus/slot/function for a vendor & device id pair
 *
 * @param vendor_id PCI vendor id
 * @param device_id PCI device id
 * @param out       Filled with the device location when found
 * @return true if found
 */
bool pci_find_device(uint16_t vendor_id, uint16_t device_id, struct PCIAddress *out);

#endif
