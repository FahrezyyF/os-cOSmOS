#include "header/driver/pci.h"
#include "header/cpu/portio.h"

uint32_t pci_config_read32(struct PCIAddress addr, uint8_t offset) {
    uint32_t address = 0x80000000u
                     | ((uint32_t) addr.bus << 16)
                     | ((uint32_t) (addr.slot & 0x1F) << 11)
                     | ((uint32_t) (addr.function & 0x7) << 8)
                     | (offset & 0xFC);
    out32(PCI_CONFIG_ADDRESS, address);
    return in32(PCI_CONFIG_DATA);
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, struct PCIAddress *out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t function = 0; function < 8; function++) {
                struct PCIAddress addr = {.bus = (uint8_t) bus, .slot = slot, .function = function};
                uint32_t id = pci_config_read32(addr, 0x00);
                if ((id & 0xFFFF) == PCI_VENDOR_NONE) {
                    if (function == 0)
                        break; // No device in this slot
                    continue;
                }
                if ((id & 0xFFFF) == vendor_id && (id >> 16) == device_id) {
                    *out = addr;
                    return true;
                }
                // Only scan functions 1-7 for multi-function devices (header type bit 7)
                if (function == 0 && !(pci_config_read32(addr, 0x0C) & 0x00800000))
                    break;
            }
        }
    }
    return false;
}
