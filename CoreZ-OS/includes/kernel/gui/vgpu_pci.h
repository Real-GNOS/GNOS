#ifndef NT_VGPU_PCI_H
#define NT_VGPU_PCI_H

#include <stdint.h>
#include "drivers/net/pci.h"

#define VIRTIO_F_VERSION_1 (1ULL << 32)

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8

#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2

static inline uint32_t vg_mmio_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void vg_mmio_write32(volatile uint8_t *base, uint32_t off,
                                   uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static inline uint16_t vg_mmio_read16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}

static inline void vg_mmio_write16(volatile uint8_t *base, uint32_t off,
                                   uint16_t v) {
    *(volatile uint16_t *)(base + off) = v;
}

static inline void vg_mmio_write8(volatile uint8_t *base, uint32_t off,
                                  uint8_t v) {
    *(volatile uint8_t *)(base + off) = v;
}

#endif
