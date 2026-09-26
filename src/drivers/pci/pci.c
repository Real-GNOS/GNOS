/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pci.c — minimal PCI bus enumeration. (GPLv2)
 *
 * See pci.h for the type-1 access scheme.  We only scan bus 0, which is where
 * QEMU places every device we ask for (-device e1000, -device ac97).  Each
 * function found is recorded with its identity and raw BARs; the drivers do
 * the rest.
 */
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "debugcon.h"
#include "idt.h"
#define PCI_PAGE_SIZE 4096       /* irq_handler_t, irq_install */

#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC

pci_dev_t g_pci_devs[PCI_MAX_DEVICES];
int       g_pci_count;

static uint32_t cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (reg & 0xFC);
    outl(PCI_ADDR_PORT, addr);
    return inl(PCI_DATA_PORT);
}

static void cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                        uint32_t val)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (reg & 0xFC);
    outl(PCI_ADDR_PORT, addr);
    outl(PCI_DATA_PORT, val);
}

const pci_dev_t *pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < g_pci_count; i++)
        if (g_pci_devs[i].vendor == vendor && g_pci_devs[i].device == device)
            return &g_pci_devs[i];
    return NULL;
}

const pci_dev_t *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < g_pci_count; i++)
        if (g_pci_devs[i].class_code == class_code &&
            g_pci_devs[i].subclass == subclass)
            return &g_pci_devs[i];
    return NULL;
}

static uint64_t bar_phys_and_size(const pci_dev_t *d, int idx, uint64_t *size_out)
{
    uint32_t lo = d->bar[idx];
    *size_out = 0;
    if (lo & 0x1)
        return 0;                       /* I/O BAR: not memory-mapped */

    uint8_t  reg  = (uint8_t)(0x10 + idx * 4);
    uint32_t orig = cfg_read32(d->bus, d->dev, d->fn, reg);

    /* Sizing moves the BAR while it is in progress, so decoding has to be off
     * or the device briefly answers at an address nobody asked about. */
    uint32_t cmd = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd & ~0x3u);

    /* The size is the number of low-order address bits the device hard-wires
     * to zero, learned by writing all-ones and seeing what sticks. */
    cfg_write32(d->bus, d->dev, d->fn, reg, 0xFFFFFFFF);
    uint32_t rb = cfg_read32(d->bus, d->dev, d->fn, reg);
    cfg_write32(d->bus, d->dev, d->fn, reg, orig);
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd);

    uint64_t base = (uint64_t)lo & ~0xFULL;
    uint32_t mask = rb & 0xFFFFFFF0u;
    if (mask == 0)
        return base;                    /* BAR not implemented: size stays 0 */

    /* Keep this arithmetic in 32 bits.  Complementing the mask *after* it has
     * been widened to 64 sets all 32 upper bits, and the resulting "size" of
     * ~2^52 pages will happily consume every free frame in the machine
     * building page tables before anything notices. */
    uint64_t size = (uint64_t)(uint32_t)(~mask) + 1;

    /* 64-bit BAR: the next config dword holds the high 32 bits of the address.
     * We do not probe the high half for size -- nothing QEMU gives us needs a
     * region larger than 4 GiB, and a wrong size here would map the world. */
    if ((lo & 0x7) == 0x4)
        base |= ((uint64_t)d->bar[idx + 1]) << 32;

    *size_out = size;
    return base;
}

uint16_t pci_bar_io(const pci_dev_t *d, int idx)
{
    if (idx < 0 || idx > 5)
        return 0;
    uint32_t v = d->bar[idx];
    if (!(v & 0x1))
        return 0;                       /* memory BAR, not an I/O one */
    return (uint16_t)(v & ~0x3u);
}

void pci_enable(const pci_dev_t *d)
{
    uint32_t cmd = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x7;                 /* I/O space | memory space | bus master */
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}

uint64_t pci_map_bar(const pci_dev_t *d, int idx)
{
    if (idx < 0 || idx > 5)
        return 0;
    uint64_t size;
    uint64_t phys = bar_phys_and_size(d, idx, &size);
    if (!phys || !size)
        return 0;
    return vmm_map_mmio(phys, size);
}

/* Offset of the MSI capability in this function's config space, or 0. */
static uint8_t pci_msi_cap_offset(const pci_dev_t *d)
{
    uint32_t st = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    uint8_t  ptr = (uint8_t)(cfg_read32(d->bus, d->dev, d->fn, 0x34) & 0xFF);
    if (!(st & (1u << 20)))
        return 0;
    while (ptr) {
        uint32_t cw = cfg_read32(d->bus, d->dev, d->fn, ptr & 0xFC);
        if ((uint8_t)(cw & 0xFF) == PCI_CAP_ID_MSI)
            return ptr;
        ptr = (uint8_t)((cw >> 8) & 0xFF);
    }
    return 0;
}

/* MSI vectors come from the dedicated 0xE0..0xEF pool. */
static uint8_t g_msi_next = 0xE0;

/* A name slot per vector for /proc/interrupts: "PCI-MSI 00:03.0" etc. */
static char g_vec_name[16][24];

static void vec_set_name(unsigned slot, const char *kind, const pci_dev_t *d)
{
    char *p = g_vec_name[slot];
    const char *q = kind;
    while (*q)
        *p++ = *q++;
    *p++ = ' ';
    static const char hexd[] = "0123456789abcdef";
    *p++ = hexd[d->bus >> 4];
    *p++ = hexd[d->bus & 0xF];
    *p++ = ':';
    *p++ = hexd[d->dev >> 4];
    *p++ = hexd[d->dev & 0xF];
    *p++ = '.';
    *p++ = (char)('0' + d->fn);
    *p = 0;
}

static uint8_t pci_msi_alloc_vector(void)
{
    uint8_t v = g_msi_next;
    g_msi_next++;
    if (g_msi_next > 0xEF)
        g_msi_next = 0xE0;               /* wrap: callers are few */
    return v;
}

#define MSI_ADDR_APIC 0xFEE00000ULL      /* the BSP's LAPIC, destination 0 */

/* Put `d` on message-signalled interrupts: allocate a vector, point the
 * device's MSI capability at the BSP LAPIC, enable it and turn INTx off.
 * A device without an MSI capability falls back to its INTx line, which is
 * what Linux does for old hardware too.  Returns the vector in *out_vec. */
int pci_enable_msi(const pci_dev_t *d, pci_msi_handler_t handler, uint8_t *out_vec)
{
    uint8_t ptr = pci_msi_cap_offset(d);
    if (!ptr) {
        /* No MSI: try MSI-X (QEMU's NVMe carries only that one), then the
         * INTx line as the last resort for old hardware. */
        if (pci_enable_msix(d, handler, out_vec) == 0)
            return 0;
        vec_set_name(d->irq_line, "PCI-INTx", d);
        irq_install(d->irq_line, (irq_handler_t)handler,
                    g_vec_name[d->irq_line]);
        *out_vec = (uint8_t)(0x20 + d->irq_line);
        return 0;
    }

    uint32_t ctrl_w = cfg_read32(d->bus, d->dev, d->fn, ptr + 0x0);
    uint16_t ctrl = (uint16_t)((ctrl_w >> 16) & 0xFFFF);
    int msi64 = (ctrl >> 7) & 1;

    uint8_t vector = pci_msi_alloc_vector();
    vec_set_name(vector - MSI_VECTOR_BASE, "PCI-MSI", d);
    msi_install(vector, (irq_handler_t)handler, g_vec_name[vector - MSI_VECTOR_BASE]);

    cfg_write32(d->bus, d->dev, d->fn, ptr + 0x4, MSI_ADDR_APIC);
    if (msi64)
        cfg_write32(d->bus, d->dev, d->fn, ptr + 0x8, 0);
    cfg_write32(d->bus, d->dev, d->fn,
                ptr + (msi64 ? 0xC : 0x8), (uint32_t)vector);

    ctrl |= 0x0001;                      /* MSI enable */
    cfg_write32(d->bus, d->dev, d->fn, ptr + 0x0,
                (ctrl_w & 0xFFFF) | ((uint32_t)ctrl << 16));

    /* INTx disable (bit 10) on top of the usual enables. */
    uint32_t cmd = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x7 | (1u << 10);
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd);

    *out_vec = vector;
    return 0;
}

/* MSI-X: the table lives inside one of the device's own BARs (BIR says
 * which, the offset comes with it), and each entry is 16 bytes of
 * message-address/-data/vector-control.  The BAR is already assigned by
 * the firmware; map the page holding the table and write entry 0. */
int pci_enable_msix(const pci_dev_t *d, pci_msi_handler_t handler,
                    uint8_t *out_vec)
{
    uint8_t ptr = 0;
    uint32_t st = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    uint8_t  cp = (uint8_t)(cfg_read32(d->bus, d->dev, d->fn, 0x34) & 0xFF);
    if (!(st & (1u << 20)))
        return -1;
    while (cp) {
        uint32_t cw = cfg_read32(d->bus, d->dev, d->fn, cp & 0xFC);
        if ((uint8_t)(cw & 0xFF) == PCI_CAP_ID_MSIX) {
            ptr = cp;
            break;
        }
        cp = (uint8_t)((cw >> 8) & 0xFF);
    }
    if (!ptr)
        return -1;

    uint32_t ctrl_w = cfg_read32(d->bus, d->dev, d->fn, ptr + 0x0);
    uint16_t ctrl = (uint16_t)((ctrl_w >> 16) & 0xFFFF);
    uint32_t toff = cfg_read32(d->bus, d->dev, d->fn, ptr + 0x4);
    uint8_t  bir = (uint8_t)(toff & 0x7);
    uint64_t toff_bytes = toff & ~0x7ULL;

    uint64_t bar_phys = d->bar[bir] & ~0xFULL;
    uint64_t table_va = vmm_map_mmio(bar_phys + toff_bytes, PCI_PAGE_SIZE);
    if (!table_va)
        return -1;

    uint8_t vector = pci_msi_alloc_vector();
    vec_set_name(vector - MSI_VECTOR_BASE, "PCI-MSI-X", d);
    msi_install(vector, handler, g_vec_name[vector - MSI_VECTOR_BASE]);

    volatile uint32_t *entry = (volatile uint32_t *)(uintptr_t)table_va;
    entry[0] = 0xFEE00000u;              /* message address: the BSP LAPIC */
    entry[1] = 0;                        /* upper address (32-bit dest)    */
    entry[2] = (uint32_t)vector;         /* message data: the vector       */
    entry[3] = 0;                        /* vector control: unmasked       */

    ctrl |= (1u << 15);                  /* MSI-X enable                   */
    ctrl &= ~(1u << 14);                 /* clear function mask            */
    cfg_write32(d->bus, d->dev, d->fn, ptr + 0x0,
                (ctrl_w & 0xFFFF) | ((uint32_t)ctrl << 16));

    uint32_t cmd = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x7 | (1u << 10);             /* enable spaces, disable INTx    */
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd);

    *out_vec = vector;
    return 0;
}

/* Walk the capability list at offset 0x34 and record the ids we find. */
static void pci_read_caps(pci_dev_t *d)
{
    uint32_t st = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    uint8_t  ptr = (uint8_t)(cfg_read32(d->bus, d->dev, d->fn, 0x34) & 0xFF);
    int n = 0;
    /* The list is only valid when the status register says it exists. */
    if (!(st & (1u << 20)))
        ptr = 0;
    while (ptr && n < 8) {
        uint32_t cw = cfg_read32(d->bus, d->dev, d->fn, ptr & 0xFC);
        uint8_t id = (uint8_t)(cw & 0xFF);
        d->caps[n++] = id;
        ptr = (uint8_t)((cw >> 8) & 0xFF);
        if (ptr == (uint8_t)(ptr & 0xFC) && id == 0)
            break;                      /* malformed list: stop */
    }
    d->n_caps = (uint8_t)n;
}

int pci_has_cap(const pci_dev_t *d, uint8_t cap_id)
{
    if (!d)
        return 0;
    for (int i = 0; i < d->n_caps; i++)
        if (d->caps[i] == cap_id)
            return 1;
    return 0;
}

static void pci_scan_bus(uint8_t bus);

/* A PCI-to-PCI bridge (header type 1) opens a new bus: its primary,
 * secondary and subordinate bus numbers say which range hangs off it, and
 * everything in that range has to be scanned too -- this is how a full
 * topology (not just bus 0) is discovered. */
static void pci_probe_bridge(pci_dev_t *d)
{
    uint32_t bl = cfg_read32(d->bus, d->dev, d->fn, 0x18);
    uint8_t secondary   = (uint8_t)((bl >> 8) & 0xFF);
    uint8_t subordinate = (uint8_t)((bl >> 16) & 0xFF);
    if (!secondary)
        return;                          /* not configured yet */
    for (unsigned b = secondary; b <= subordinate && b < 256; b++)
        pci_scan_bus((uint8_t)b);
}

static void pci_scan_bus(uint8_t bus)
{
    static int depth;
    if (depth > 8)                       /* topology guard */
        return;
    depth++;
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = cfg_read32(bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF)
                    continue;           /* no device here */

                if (g_pci_count >= PCI_MAX_DEVICES)
                    break;

                pci_dev_t *d = &g_pci_devs[g_pci_count];
                uint32_t cls = cfg_read32(bus, dev, fn, 0x08);
                d->bus      = bus;
                d->dev      = dev;
                d->fn       = fn;
                d->vendor   = vendor;
                d->device   = (uint16_t)(id >> 16);
                d->progif   = (uint8_t)(cls >> 8);
                d->subclass = (uint8_t)(cls >> 16);
                d->class_code = (uint8_t)(cls >> 24);
                d->hdr_type = (uint8_t)(cfg_read32(bus, dev, fn, 0x0C) >> 16);
                for (int i = 0; i < 6; i++)
                    d->bar[i] = cfg_read32(bus, dev, fn, (uint8_t)(0x10 + i * 4));
                uint32_t il = cfg_read32(bus, dev, fn, 0x3C);
                d->irq_line = (uint8_t)(il & 0xFF);
                d->irq_pin  = (uint8_t)((il >> 8) & 0xFF);
                g_pci_count++;

                pci_read_caps(d);

                /* A device with an MSI capability goes onto message
                 * interrupts right away: the handler is a stub for now,
                 * the point is that the config writes stick and the
                 * vector pool allocates. */
                if (pci_msi_cap_offset(d) || pci_enable_msix(d, 0, &(uint8_t){0}) == 0) {
                    uint8_t vec = 0;
                    if (pci_enable_msi(d, 0, &vec) == 0) {
                        dbg_puts("PCI: MSI enabled on ");
                        dbg_puts_hexn(d->bus, 2); dbg_puts(":");
                        dbg_puts_hexn(d->dev, 2); dbg_puts(".");
                        dbg_puts_hexn(d->fn, 1);  dbg_puts(" vec=");
                        dbg_puts_hexn(vec, 2);
                        dbg_puts("\r\n");
                    }
                }

                /* A bridge opens a downstream bus: recurse into it. */
                if ((d->hdr_type & 0x7F) == 0x01)
                    pci_probe_bridge(d);

                /* Stop after fn 0 if the device is not multifunction. */
                if (fn == 0) {
                    uint8_t hdr = d->hdr_type;
                    if (!(hdr & 0x80)) {
                        fn = 7;        /* break out of the fn loop */
                    }
                }
            }
        }
    depth--;
}

void pci_init(void)
{
    g_pci_count = 0;
    pci_scan_bus(0);

    dbg_puts("PCI: scanned topology, ");
    dbg_puts_dec((uint32_t)g_pci_count);
    dbg_puts(" device(s):\r\n");
    for (int i = 0; i < g_pci_count; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        dbg_puts("  ");
        dbg_puts_hexn(d->bus, 2); dbg_puts(":");
        dbg_puts_hexn(d->dev, 2); dbg_puts(".");
        dbg_puts_hexn(d->fn, 1);  dbg_puts("  ");
        dbg_puts_hexn(d->vendor, 4); dbg_puts(":");
        dbg_puts_hexn(d->device, 4); dbg_puts("  class ");
        dbg_puts_hexn(d->class_code, 2); dbg_puts("/");
        dbg_puts_hexn(d->subclass, 2);
        if (d->n_caps) {
            dbg_puts(" caps:");
            for (int k = 0; k < d->n_caps; k++) {
                dbg_puts(" ");
                dbg_puts_hexn(d->caps[k], 2);
            }
        }
        dbg_puts("\r\n");
    }
}
