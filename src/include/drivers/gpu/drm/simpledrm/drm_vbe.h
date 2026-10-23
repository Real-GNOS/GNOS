/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_vbe.h - the Bochs VBE_DISPI register interface. (GPLv2)
 *
 * QEMU's stdvga and bochs-display both implement these registers.  Modesetting
 * goes through them; the framebuffer itself stays where the bootloader put it,
 * so fbcon keeps drawing into the same memory and only its geometry changes.
 */

#ifndef INCLUDE_DRM_SIMPLEDRM_DRM_VBE_H_
#define INCLUDE_DRM_SIMPLEDRM_DRM_VBE_H_

#include <stdint.h>

#define VBE_PORT_IDX 0x01CE
#define VBE_PORT_DAT 0x01CF

#define VBE_ID     0x00
#define VBE_XRES   0x01
#define VBE_YRES   0x02
#define VBE_BPP    0x03
#define VBE_ENABLE 0x04

#define VBE_ENABLE_LFB 0x41 /* enable + linear framebuffer */

/* Locate the display device through the PCI scan, switch it on and probe the
 * register bank (MMIO BAR first, I/O ports as the fallback).  Returns the ID
 * register value when the interface answered, else 0. */
int vbe_pci_probe(void);

void     vbe_write(uint16_t idx, uint16_t val);
uint16_t vbe_read(uint16_t idx);

#endif /* INCLUDE_DRM_SIMPLEDRM_DRM_VBE_H_ */
