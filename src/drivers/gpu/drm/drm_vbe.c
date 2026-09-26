/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_vbe.c — the bochs VBE_DISPI register interface and current scanout
 * state. (GPLv2)
 *
 * Modesetting goes through the Bochs VBE_DISPI registers (I/O 0x1CE/0x1CF),
 * the interface QEMU's stdvga and bochs-display have both implemented for
 * twenty-five years.  The framebuffer stays where it always was -- the
 * linear BAR0 the bootloader set up -- so fbcon keeps drawing into the same
 * memory and only its geometry changes.
 */
#include <stdint.h>

#include "drm.h"
#include "drm_internal.h"
#include "drm_mode.h"
#include "debugcon.h"
#include "io.h"
#include "pci.h"

int g_vbe;                              /* VBE_DISPI answered the ID probe */

/* When the register bank is memory-mapped (bochs-display puts it in an
 * MMIO BAR; stdvga keeps it in I/O ports), accesses go here at 16-bit
 * stride: register `idx` lives at offset idx * 2, exactly as the Linux
 * bochs driver maps it. */
static volatile uint16_t *g_vbe_mmio;

#define VBE_ID_SIGNATURE 0xB0C0         /* ID registers read 0xB0C0..0xB0C5 */
#define PCI_CLASS_DISPLAY 0x03
#define PCI_SUBCLASS_VGA  0x00

/* Locate the display device through the PCI scan, switch it on, and probe
 * the VBE interface: every memory BAR first (MMIO variant), then the
 * classic I/O ports.  A signature answer arms g_vbe so mode changes may
 * use the registers. */
int vbe_pci_probe(void)
{
    const pci_dev_t *d = pci_find_class(PCI_CLASS_DISPLAY, PCI_SUBCLASS_VGA);
    if (!d)
        return 0;
    pci_enable(d);

    for (int i = 0; i < 6; i++) {
        uint32_t raw = d->bar[i];
        if (!raw || (raw & 1))          /* I/O BARs: tried below */
            continue;
        uint64_t va = pci_map_bar(d, i);
        if (!va)
            continue;
        volatile uint16_t *mm = (volatile uint16_t *)(uintptr_t)va;
        uint16_t id = mm[VBE_ID];
        if ((id & 0xFFF0) == VBE_ID_SIGNATURE) {
            g_vbe_mmio = mm;
            g_vbe = 1;
            dbg_puts("drm: vbe via PCI MMIO bar\r\n");
            return id;
        }
    }

    uint16_t id = vbe_read(VBE_ID);
    if ((id & 0xFFF0) == VBE_ID_SIGNATURE) {
        g_vbe = 1;
        dbg_puts("drm: vbe via PCI ioports\r\n");
        return id;
    }
    return 0;
}

/* ---- current scanout state ------------------------------------------------ */
uint32_t g_cur_w, g_cur_h;              /* current mode */
uint32_t g_cur_fb;                      /* fb id on screen, 0 = console owns it */

void vbe_write(uint16_t idx, uint16_t val)
{
    if (g_vbe_mmio) {
        g_vbe_mmio[idx] = val;
        return;
    }
    outw(VBE_PORT_IDX, idx);
    outw(VBE_PORT_DAT, val);
}

uint16_t vbe_read(uint16_t idx)
{
    if (g_vbe_mmio)
        return g_vbe_mmio[idx];
    outw(VBE_PORT_IDX, idx);
    return inw(VBE_PORT_DAT);
}
