#include "drivers/pci/pci.h"

#include "kernel/asm_func.h"

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu
#define PCI_ENABLE_BIT 0x80000000u
#define PCI_DEV_NR 32u

static uint32_t pci_addr_of(uint32_t bdf, uint8_t reg) {
    return PCI_ENABLE_BIT | PCI_BDF_BUS(bdf) << 16 |
           PCI_BDF_DEV(bdf) << 11 | PCI_BDF_FUNC(bdf) << 8 | (reg & 0xFCu);
}

uint32_t pci_config_read32(uint32_t bdf, uint8_t reg) {
    outl(PCI_CONFIG_ADDR, pci_addr_of(bdf, reg));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint32_t bdf, uint8_t reg, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr_of(bdf, reg));
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_config_read16(uint32_t bdf, uint8_t reg) {
    uint32_t aligned = pci_addr_of(bdf, (uint8_t)(reg & 0xFCu));
    uint32_t shift = (reg & 2u) ? 16u : 0u;

    outl(PCI_CONFIG_ADDR, aligned);
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> shift);
}

int pci_find_class(uint8_t base_class, uint8_t subclass, uint32_t *bdf) {
    for (uint32_t dev = 0; dev < PCI_DEV_NR; dev++) {
        uint32_t slot = PCI_BDF(0, dev, 0);
        uint32_t vendor = pci_config_read32(slot, PCI_CFG_VENDOR);
        if ((vendor & 0xFFFFu) == 0xFFFFu) {
            continue;
        }
        uint32_t class = pci_config_read32(slot, PCI_CFG_CLASS);
        uint8_t cls = (uint8_t)(class >> 24);
        uint8_t sub = (uint8_t)(class >> 16);
        if (cls == base_class && sub == subclass) {
            *bdf = slot;
            return 0;
        }
    }
    return -1;
}

void pci_cmd_set(uint32_t bdf, uint32_t bits) {
    uint32_t cmd = pci_config_read32(bdf, PCI_CFG_CMD);

    pci_config_write32(bdf, PCI_CFG_CMD, cmd | bits);
}
