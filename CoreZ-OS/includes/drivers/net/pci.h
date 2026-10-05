#ifndef NT_PCI_H
#define NT_PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline void pci_outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t reg) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 7) << 11) | ((uint32_t)dev & ~7u) |
                    (reg & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    return pci_inl(PCI_CONFIG_DATA);
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t reg, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 7) << 11) | ((uint32_t)dev & ~7u) |
                    (reg & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    pci_outl(PCI_CONFIG_DATA, val);
}

static inline uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t reg) {
    uint32_t v = pci_read32(bus, dev, reg & 0xFC);
    return (uint16_t)((reg & 2) ? (v >> 16) : v);
}

static inline void pci_write16(uint8_t bus, uint8_t dev, uint8_t reg, uint16_t val) {
    uint32_t old = pci_read32(bus, dev, reg & 0xFC);
    uint32_t v = (reg & 2) ? ((old & 0xFFFF) | ((uint32_t)val << 16))
                           : ((old & 0xFFFF0000) | val);
    pci_write32(bus, dev, reg & 0xFC, v);
}

static inline int pci_find_device(uint16_t vendor, uint16_t device,
                                  uint8_t* bus, uint8_t* dev) {
    for (uint16_t d = 0; d < 32; d++) {
        uint8_t b = 0;
        uint32_t id = pci_read32(b, (uint8_t)d, 0);
        if (id == 0xFFFFFFFFu || id == 0) continue;
        if (((id & 0xFFFF) == vendor) && (((id >> 16) & 0xFFFF) == device)) {
            if (bus) *bus = b;
            if (dev) *dev = (uint8_t)d;
            return 1;
        }
    }
    return 0;
}

static inline void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint16_t flags) {
    uint16_t cmd = pci_read16(bus, dev, 0x04);
    pci_write16(bus, dev, 0x04, cmd | flags);
}

#endif
