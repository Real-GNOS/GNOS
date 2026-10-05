#ifndef DRIVERS_PCI_PCI_H
#define DRIVERS_PCI_PCI_H

#include <stdint.h>

#define PCI_CFG_VENDOR 0x00u
#define PCI_CFG_CMD 0x04u
#define PCI_CFG_STATUS 0x06u
#define PCI_CFG_CLASS 0x08u
#define PCI_CFG_BAR0 0x10u

#define PCI_CMD_IO (1u << 0)
#define PCI_CMD_MEM (1u << 1)
#define PCI_CMD_BUS_MASTER (1u << 2)

#define PCI_CLASS_MASS_STORAGE 0x01u
#define PCI_SUBCLASS_NVM 0x08u

#define PCI_BDF(bus, dev, func)                                               \
    (((uint32_t)(bus) << 16) | ((uint32_t)(dev) << 11) | ((uint32_t)(func) << 8))
#define PCI_BDF_BUS(bdf) ((uint8_t)((bdf) >> 16))
#define PCI_BDF_DEV(bdf) ((uint8_t)(((bdf) >> 11) & 0x1Fu))
#define PCI_BDF_FUNC(bdf) ((uint8_t)(((bdf) >> 8) & 0x7u))

uint32_t pci_config_read32(uint32_t bdf, uint8_t reg);
void pci_config_write32(uint32_t bdf, uint8_t reg, uint32_t val);
uint16_t pci_config_read16(uint32_t bdf, uint8_t reg);
int pci_find_class(uint8_t base_class, uint8_t subclass, uint32_t *bdf);
void pci_cmd_set(uint32_t bdf, uint32_t bits);

#endif
