/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ahci.c — the AHCI controller: probe, port bring-up, IDENTIFY, and the
 * 48-bit DMA read/write command set. (GPLv2)
 *
 * The bring-up sequence, which is where every AHCI driver earns its bugs:
 *
 *   find the controller by class (01/06, prog-if 01 = AHCI, not the
 *   vendor-specific SATA controllers that share the subclass), enable
 *   memory space and bus mastering, map the BAR;
 *
 *   take the HBA out of BIOS ownership if the firmware still holds it
 *   (HOST_CAP2 bit 0 advertises the hand-off, HOST_BOHC bit 0 the ownership);
 *
 *   HBA reset: HOST_CTL.GHC.RST, wait for it to self-clear, then set
 *   HOST_CTL.AE and wait for the bit to report back;
 *
 *   per port that HOST_PORTS_IMPL claims: skip it when its PHY reports no
 *   device (DET=0) rather than spending 100 ms of reset-and-poll on an
 *   empty cable, allocate the command memory, stop the port engine, assert
 *   and deassert COMRESET, wait for the PHY to report DET=3, start the
 *   engine, and read PORT_SIG to learn what is on the wire.
 *
 * An ATA disk then answers IDENTIFY DEVICE and lands in the device table;
 * a 0xEB140101 signature means a CD-ROM, which is handed to satapi.c with
 * the port's ATAPI bit set.
 *
 * The command path (ahci_issue_cmd) is deliberately single-table: one
 * command table and one PRDT per port, one command in flight at a time,
 * completion polled through PORT_CI.  That is slower than NCQ's thirty-two
 * outstanding commands, but it is also one shared resource instead of
 * thirty-two, and the per-port lock in the port state is the whole
 * synchronisation story.
 *
 * The register protocol follows Uinxed's drivers/block/ata/sata/ahci.c;
 * the allocation and logging use GNOS' frame allocator and debug console.
 */
#include <stdint.h>

#include "ahci.h"
#include "satapi.h"
#include "pci.h"
#include "pmm.h"
#include "timer.h"
#include "vfs.h"
#include "kstring.h"
#include "debugcon.h"
#include "io.h"

/* Bounce buffer per port: 8 frames, one DMA command per 64 sectors. */
#define SATA_DMA_BUF_PAGES   8
#define SATA_DMA_MAX_SECTORS (SATA_DMA_BUF_PAGES * 4096 / 512)

/* Global device tables */
ahci_device_t ahci_devices[AHCI_MAX_DEVICES];
int           ahci_device_count = 0;

/* HBA memory base */
volatile uint8_t *hba_mmio = 0;

/* Per-port state */
ahci_port_state_t ahci_ports[AHCI_MAX_PORTS];

/* Number of ports that came up with a device */
static int ahci_port_count = 0;

/* MMIO helpers */
uint32_t ahci_read32(volatile uint8_t *base, uint32_t reg)
{
    return *(volatile uint32_t *)(base + reg);
}

/* Write a 32-bit AHCI register. */
void ahci_write32(volatile uint8_t *base, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(base + reg) = val;
}

/* Slot finder */
int ahci_find_slot(ahci_port_state_t *port)
{
    uint32_t slots = ((ahci_read32(hba_mmio, HOST_CAP) >> 8) & 0x1F) + 1;
    uint32_t ci    = ahci_read32(port->port_mmio, PORT_CI);
    uint32_t sact  = ahci_read32(port->port_mmio, PORT_SACT);

    for (uint32_t i = 0; i < slots; i++)
        if (!((ci | sact) & (1u << i)))
            return (int)i;
    return -E_BUSY;
}

/* Port start / stop */
static int ahci_port_stop(ahci_port_state_t *port)
{
    int               tout;
    volatile uint8_t *p = port->port_mmio;

    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) & ~PORT_CMD_ST);

    tout = 100000;
    while (ahci_read32(p, PORT_CMD) & PORT_CMD_CR) {
        if (--tout <= 0)
            return -E_AGAIN;
        io_delay();
    }

    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) & ~PORT_CMD_FRE);

    tout = 100000;
    while (ahci_read32(p, PORT_CMD) & PORT_CMD_FR) {
        if (--tout <= 0)
            return -E_AGAIN;
        io_delay();
    }

    return 0;
}

/* AHCI port start. */
static int ahci_port_start(ahci_port_state_t *port)
{
    volatile uint8_t *p = port->port_mmio;
    int               tout;

    if (ahci_port_stop(port) != 0)
        return -E_AGAIN;

    ahci_write32(p, PORT_LST_ADDR, (uint32_t)(port->clb_phys & 0xFFFFFFFFULL));
    ahci_write32(p, PORT_LST_ADDR_HI, (uint32_t)(port->clb_phys >> 32));
    ahci_write32(p, PORT_FIS_ADDR, (uint32_t)(port->fb_phys & 0xFFFFFFFFULL));
    ahci_write32(p, PORT_FIS_ADDR_HI, (uint32_t)(port->fb_phys >> 32));
    ahci_write32(p, PORT_SERR, 0xFFFFFFFF);
    ahci_write32(p, PORT_IRQ_STAT, 0xFFFFFFFF);
    ahci_write32(p, PORT_IRQ_MASK, 0);
    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) | PORT_CMD_FRE);

    tout = 100000;
    while (!(ahci_read32(p, PORT_CMD) & PORT_CMD_FR)) {
        if (--tout <= 0)
            return -E_AGAIN;
        io_delay();
    }

    ahci_write32(p, PORT_CMD, ahci_read32(p, PORT_CMD) | PORT_CMD_ST);

    tout = 100000;
    while (!(ahci_read32(p, PORT_CMD) & PORT_CMD_CR)) {
        if (--tout <= 0)
            return -E_AGAIN;
        io_delay();
    }

    return 0;
}

/* AHCI issue cmd. */
static int ahci_issue_cmd(ahci_port_state_t *port, int slot, uint8_t *cfis, int write, uint64_t buf_phys, uint32_t byte_count)
{
    volatile hba_cmd_header_t *hdr = &port->cmd_list[slot];
    volatile uint8_t          *p   = port->port_mmio;
    int                        tout;

    hdr->cfl   = CFL_DWORDS;
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = byte_count ? 1 : 0;
    hdr->p     = 1;

    memcpy((void *)port->cmd_tbl->cfis, cfis, 5 * sizeof(uint32_t));

    if (byte_count) {
        volatile hba_prdt_entry_t *prdt = &port->cmd_tbl->prdt_entry[0];
        prdt->dba                       = (uint32_t)(buf_phys & 0xFFFFFFFFULL);
        prdt->dbau                      = (uint32_t)(buf_phys >> 32);
        prdt->dbc                       = byte_count - 1;
        prdt->i                         = 1;
    }

    hdr->ctba  = (uint32_t)(port->ct_phys & 0xFFFFFFFFULL);
    hdr->ctbau = (uint32_t)(port->ct_phys >> 32);

    tout = 100000;
    while (ahci_read32(p, PORT_TFDATA) & 0x88) {
        if (--tout <= 0)
            return -E_AGAIN;
        io_delay();
    }

    ahci_write32(p, PORT_CI, (uint32_t)(1 << slot));

    tout = 1000000;
    while (1) {
        if (!(ahci_read32(p, PORT_CI) & (uint32_t)(1 << slot)))
            break;
        if (ahci_read32(p, PORT_IRQ_STAT) & PORT_IRQ_TF_ERR) {
            ahci_write32(p, PORT_IRQ_STAT, PORT_IRQ_TF_ERR);
            return -E_IO;
        }
        if (--tout <= 0)
            return -E_AGAIN;
        io_delay();
    }

    if (ahci_read32(p, PORT_TFDATA) & 0x01)
        return -E_IO;
    return 0;
}

/* Identify device (SATA only after signature check).  The drive answers
 * with 256 words; capacity is LBA48 when word 83 bit 10 says so, and the
 * model string is words 27..46 stored big-endian within each word. */
static int ahci_port_identify(ahci_port_state_t *port, ahci_device_t *dev)
{
    fis_reg_h2d_t cfis;
    int           slot, ret;

    memset(&cfis, 0, sizeof(cfis));
    cfis.fis_type = FIS_TYPE_REG_H2D;
    cfis.c        = 1;
    cfis.command  = ATA_CMD_IDENTIFY;
    cfis.device   = 0;

    slot = ahci_find_slot(port);
    if (slot < 0)
        return -E_BUSY;

    memset(port->dma_buf, 0, 512);

    ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 0, port->dma_buf_phys, 512);
    if (ret != 0)
        return ret;

    uint16_t *buf = (uint16_t *)port->dma_buf;
    if (buf[0] == 0x0000 || buf[0] == 0xFFFF)
        return -E_NODEV;

    dev->reserved    = 1;
    dev->type        = AHCI_DEV_SATA;
    dev->sector_size = ATA_SECTOR;

    uint16_t *ident = (uint16_t *)port->dma_buf;
    uint32_t  cmds  = (uint32_t)ident[82] | ((uint32_t)ident[83] << 16);

    if (cmds & (1u << 26)) {
        dev->size = (uint64_t)ident[100] | ((uint64_t)ident[101] << 16) |
                    ((uint64_t)ident[102] << 32) | ((uint64_t)ident[103] << 48);
    } else {
        dev->size = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    }

    for (int k = 0; k < 40; k += 2) {
        dev->model[k]     = port->dma_buf[ATA_IDENT_MODEL * 2 + k + 1];
        dev->model[k + 1] = port->dma_buf[ATA_IDENT_MODEL * 2 + k];
    }

    dev->model[40] = 0;

    for (int k = 39; k > 0; k--) {
        if (dev->model[k] == ' ') {
            dev->model[k] = '\0';
        } else {
            break;
        }
    }

    return 0;
}

/* AHCI read sectors. */
int ahci_read_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, void *buffer)
{
    if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved)
        return -E_NODEV;
    if (ahci_devices[drive].type != AHCI_DEV_SATA)
        return -E_NOSYS;

    uint8_t            port_idx = ahci_devices[drive].port;
    ahci_port_state_t *port     = &ahci_ports[port_idx];

    uint8_t *out  = (uint8_t *)buffer;
    uint8_t  left = numsects;

    /*
     * Serialise the whole transfer under the per-port lock: slot allocation,
     * the shared cmd_tbl/dma_buf, and the DMA buffer copy all race a
     * concurrent I/O on the same port (two CPUs reading different files on
     * the same disk).  Completion is polled, so no IRQ path needs the lock.
     */
    spin_lock(&port->lock);
    while (left) {
        uint8_t  chunk = (left > SATA_DMA_MAX_SECTORS) ? SATA_DMA_MAX_SECTORS : left;
        uint32_t bytes = (uint32_t)chunk * ATA_SECTOR;

        fis_reg_h2d_t cfis;
        memset(&cfis, 0, sizeof(cfis));

        cfis.fis_type = FIS_TYPE_REG_H2D;
        cfis.c        = 1;
        cfis.command  = ATA_CMD_READ_DMA_EXT;
        cfis.device   = 1 << 6;
        cfis.lba0     = (uint8_t)(lba & 0xFF);
        cfis.lba1     = (uint8_t)((lba >> 8) & 0xFF);
        cfis.lba2     = (uint8_t)((lba >> 16) & 0xFF);
        cfis.lba3     = (uint8_t)((lba >> 24) & 0xFF);
        cfis.lba4     = (uint8_t)((lba >> 32) & 0xFF);
        cfis.lba5     = (uint8_t)((lba >> 40) & 0xFF);
        cfis.countl   = chunk;

        int slot = ahci_find_slot(port);
        if (slot < 0) {
            dbg_puts("ahci: port ");
            dbg_puts_dec(port_idx);
            dbg_puts(": no free command slot for read\n");
            spin_unlock(&port->lock);
            return -E_BUSY;
        }

        int ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 0, port->dma_buf_phys, bytes);
        if (ret != 0) {
            dbg_puts("ahci: port ");
            dbg_puts_dec(port_idx);
            dbg_puts(": read error at LBA ");
            dbg_puts_hex(lba);
            dbg_puts("\n");
            spin_unlock(&port->lock);
            return ret;
        }

        memcpy(out, port->dma_buf, bytes);
        out += bytes;
        lba += chunk;
        left -= chunk;
    }
    spin_unlock(&port->lock);
    return 0;
}

/* AHCI write sectors. */
int ahci_write_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, const void *buffer)
{
    if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved)
        return -E_NODEV;
    if (ahci_devices[drive].type != AHCI_DEV_SATA)
        return -E_NOSYS;

    uint8_t            port_idx = ahci_devices[drive].port;
    ahci_port_state_t *port     = &ahci_ports[port_idx];

    const uint8_t *in   = (const uint8_t *)buffer;
    uint8_t        left = numsects;

    /* Serialise the whole transfer under the per-port lock (see read path). */
    spin_lock(&port->lock);
    while (left) {
        uint8_t  chunk = (left > SATA_DMA_MAX_SECTORS) ? SATA_DMA_MAX_SECTORS : left;
        uint32_t bytes = (uint32_t)chunk * ATA_SECTOR;

        memcpy(port->dma_buf, in, bytes);
        fis_reg_h2d_t cfis;
        memset(&cfis, 0, sizeof(cfis));

        cfis.fis_type = FIS_TYPE_REG_H2D;
        cfis.c        = 1;
        cfis.command  = ATA_CMD_WRITE_DMA_EXT;
        cfis.device   = 1 << 6;
        cfis.lba0     = (uint8_t)(lba & 0xFF);
        cfis.lba1     = (uint8_t)((lba >> 8) & 0xFF);
        cfis.lba2     = (uint8_t)((lba >> 16) & 0xFF);
        cfis.lba3     = (uint8_t)((lba >> 24) & 0xFF);
        cfis.lba4     = (uint8_t)((lba >> 32) & 0xFF);
        cfis.lba5     = (uint8_t)((lba >> 40) & 0xFF);
        cfis.countl   = chunk;

        int slot = ahci_find_slot(port);
        if (slot < 0) {
            dbg_puts("ahci: port ");
            dbg_puts_dec(port_idx);
            dbg_puts(": no free command slot for write\n");
            spin_unlock(&port->lock);
            return -E_BUSY;
        }

        int ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 1, port->dma_buf_phys, bytes);
        if (ret != 0) {
            dbg_puts("ahci: port ");
            dbg_puts_dec(port_idx);
            dbg_puts(": write error at LBA ");
            dbg_puts_hex(lba);
            dbg_puts("\n");
            spin_unlock(&port->lock);
            return ret;
        }

        in += bytes;
        lba += chunk;
        left -= chunk;
    }
    spin_unlock(&port->lock);
    return 0;
}

/* Flush the drive's write cache */
int ahci_flush_cache(uint8_t drive)
{
    fis_reg_h2d_t      cfis;
    ahci_port_state_t *port;
    int                slot;

    if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved)
        return -E_NODEV;
    if (ahci_devices[drive].type != AHCI_DEV_SATA)
        return -E_NOSYS;

    port = &ahci_ports[ahci_devices[drive].port];
    slot = ahci_find_slot(port);

    if (slot < 0)
        return -E_BUSY;

    memset(&cfis, 0, sizeof(cfis));
    cfis.fis_type = FIS_TYPE_REG_H2D;
    cfis.c        = 1;

    /* AHCI data I/O uses the 48-bit command set, so pair it with
     * FLUSH CACHE EXT. */
    cfis.command = ATA_CMD_CACHE_FLUSH_EXT;
    cfis.device  = 1 << 6;

    return ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 0, 0, 0);
}

/* Pick the BAR that holds the HBA: the spec puts it on the first 32-bit
 * memory BAR, with a 64-bit BAR the fallback.  I/O-space and unimplemented
 * entries read back as all-ones or map to nothing and are skipped by the
 * failed pci_map_bar(). */
static uint64_t ahci_map_hba(const pci_dev_t *d)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 6; i++) {
            uint32_t v = d->bar[i];
            if (v & 1)
                continue; /* I/O BAR */
            int is64 = (v & 0x7) == 0x4;
            if ((pass == 0) != is64)
                continue;
            uint64_t base = pci_map_bar(d, i);
            if (base)
                return base;
        }
    }
    return 0;
}

/* Initialize the AHCI controller and probe its ports */
void ahci_init(void)
{
    static int g_ahci_inited;
    if (g_ahci_inited)
        return;
    g_ahci_inited = 1;

    const pci_dev_t *d = pci_find_class(0x01, 0x06);
    /* Class 01/06 also covers vendor-specific SATA controllers; only
     * prog-if 01 is the AHCI register layout this driver speaks. */
    if (!d || d->progif != 0x01)
        return;

    pci_enable(d);

    uint64_t base = ahci_map_hba(d);
    if (!base) {
        dbg_puts("ahci: controller found but no usable memory BAR\n");
        return;
    }
    hba_mmio = (volatile uint8_t *)base;

    dbg_puts("ahci: controller at PCI ");
    dbg_puts_hexn(d->bus, 2);
    dbg_puts(":");
    dbg_puts_hexn(d->dev, 2);
    dbg_puts(":");
    dbg_puts_hexn(d->fn, 1);
    dbg_puts(" vendor 0x");
    dbg_puts_hexn(d->vendor, 4);
    dbg_puts(":0x");
    dbg_puts_hexn(d->device, 4);
    dbg_puts("\n");

    /* BIOS/OS handoff */
    uint32_t cap2 = ahci_read32(hba_mmio, HOST_CAP2);
    if (cap2 & (1u << 0)) {
        uint32_t bohc = ahci_read32(hba_mmio, HOST_BOHC);
        if (bohc & (1u << 0)) {
            ahci_write32(hba_mmio, HOST_BOHC, bohc | (1u << 1));
            int boh_timeout = 100000;
            while (ahci_read32(hba_mmio, HOST_BOHC) & (1u << 0)) {
                if (--boh_timeout <= 0) {
                    dbg_puts("ahci: BIOS ownership handoff timeout\n");
                    return;
                }
                io_delay();
            }
        }
    }

    /* HBA reset */
    ahci_write32(hba_mmio, HOST_CTL, (ahci_read32(hba_mmio, HOST_CTL) | HOST_AHCI_EN) & ~HOST_IRQ_EN);
    ahci_write32(hba_mmio, HOST_CTL, ahci_read32(hba_mmio, HOST_CTL) | HOST_AHCI_EN | HOST_RESET);

    int reset_timeout = 100000;
    while (ahci_read32(hba_mmio, HOST_CTL) & HOST_RESET) {
        if (--reset_timeout <= 0) {
            dbg_puts("ahci: HBA reset timeout\n");
            return;
        }
        io_delay();
    }

    ahci_write32(hba_mmio, HOST_CTL, ahci_read32(hba_mmio, HOST_CTL) | HOST_AHCI_EN);

    int ae_timeout = 100000;
    while (!(ahci_read32(hba_mmio, HOST_CTL) & HOST_AHCI_EN)) {
        if (--ae_timeout <= 0) {
            dbg_puts("ahci: AHCI enable timeout\n");
            return;
        }
        io_delay();
    }

    uint32_t pi        = ahci_read32(hba_mmio, HOST_PORTS_IMPL);
    uint32_t cap       = ahci_read32(hba_mmio, HOST_CAP);
    uint32_t max_ports = (cap & 0x1F) + 1;

    if (max_ports > AHCI_MAX_PORTS)
        max_ports = AHCI_MAX_PORTS;
    dbg_puts("ahci: CAP=0x");
    dbg_puts_hexn(cap, 8);
    dbg_puts(", PI=0x");
    dbg_puts_hexn(pi, 8);
    dbg_puts(", ");
    dbg_puts_dec((uint32_t)max_ports);
    dbg_puts(" ports implemented\n");

    ahci_port_count   = 0;
    ahci_device_count = 0;

    for (uint32_t i = 0; i < max_ports; i++) {
        if (!(pi & (1u << i)))
            continue;

        ahci_port_state_t *port = &ahci_ports[ahci_port_count];
        memset(port, 0, sizeof(*port));
        port->port_mmio = hba_mmio + 0x100 + (size_t)i * 0x80;
        port->port_no   = (uint8_t)i;

        /*
         * PI describes implemented controller ports, not attached devices.
         * Do not reset and poll a port whose PHY reports no device.
         */
        if ((ahci_read32(port->port_mmio, PORT_SSTS) & 0xF) == 0)
            continue;

        /* Allocate per-port command memory: one frame each, zeroed,
         * addressed through the direct map. */
        port->clb_phys = pmm_alloc_zeroed();
        port->fb_phys  = pmm_alloc_zeroed();
        port->ct_phys  = pmm_alloc_zeroed();

        if (!port->clb_phys || !port->fb_phys || !port->ct_phys) {
            if (port->clb_phys)
                pmm_free(port->clb_phys);
            if (port->fb_phys)
                pmm_free(port->fb_phys);
            if (port->ct_phys)
                pmm_free(port->ct_phys);
            port->clb_phys = port->fb_phys = port->ct_phys = 0;
            dbg_puts("ahci: port ");
            dbg_puts_dec((uint32_t)i);
            dbg_puts(" command memory allocation failed\n");
            continue;
        }

        port->cmd_list = (hba_cmd_header_t *)pmm_virt(port->clb_phys);
        port->fis      = (hba_fis_ring_t *)pmm_virt(port->fb_phys);
        port->cmd_tbl  = (hba_cmd_tbl_t *)pmm_virt(port->ct_phys);

        /* Per-port DMA bounce buffer: contiguous frames, the PRDT cannot
         * express a page boundary in the middle of one entry. */
        port->dma_buf_phys = pmm_alloc_contiguous(SATA_DMA_BUF_PAGES);
        if (!port->dma_buf_phys) {
            pmm_free(port->clb_phys);
            pmm_free(port->fb_phys);
            pmm_free(port->ct_phys);
            port->clb_phys = port->fb_phys = port->ct_phys = 0;
            dbg_puts("ahci: port ");
            dbg_puts_dec((uint32_t)i);
            dbg_puts(" DMA buffer allocation failed\n");
            continue;
        }

        port->dma_buf = (uint8_t *)pmm_virt(port->dma_buf_phys);
        if (ahci_port_stop(port) != 0) {
            dbg_puts("ahci: port ");
            dbg_puts_dec((uint32_t)i);
            dbg_puts(" stop failed\n");
            continue;
        }

        /* Assert then deassert COMRESET before starting the command engine.
         * The spec asks for 1 ms of assert. */
        ahci_write32(port->port_mmio, PORT_SCTL, (ahci_read32(port->port_mmio, PORT_SCTL) & ~0xFu) | 0x1);
        timer_delay_ms(1);
        ahci_write32(port->port_mmio, PORT_SCTL, ahci_read32(port->port_mmio, PORT_SCTL) & ~0xFu);

        {
            uint32_t ssts;
            int      det_timeout = 100;

            while (1) {
                ssts = ahci_read32(port->port_mmio, PORT_SSTS);
                if ((ssts & 0xF) == HBA_PORT_DET_PRESENT)
                    break;
                if (--det_timeout <= 0)
                    break;
                timer_delay_ms(1);
            }
            if ((ssts & 0xF) != HBA_PORT_DET_PRESENT) {
                dbg_puts("ahci: port ");
                dbg_puts_dec((uint32_t)i);
                dbg_puts(" device detect failed (DET=");
                dbg_puts_hexn(ssts & 0xF, 1);
                dbg_puts(")\n");
                ahci_port_stop(port);
                continue;
            }
        }

        if (ahci_port_start(port) != 0) {
            dbg_puts("ahci: port ");
            dbg_puts_dec((uint32_t)i);
            dbg_puts(" start failed\n");
            continue;
        }

        uint32_t sig = ahci_read32(port->port_mmio, PORT_SIG);
        if (sig != SATA_SIG_ATA && sig != SATA_SIG_ATAPI) {
            dbg_puts("ahci: port ");
            dbg_puts_dec((uint32_t)i);
            dbg_puts(" no device (sig=0x");
            dbg_puts_hexn(sig, 8);
            dbg_puts(")\n");
            ahci_port_stop(port);
            continue;
        }
        if (sig == SATA_SIG_ATAPI) {
            /* SATAPI device -- handled by satapi.c, just register it in
             * the device table. */
            ahci_device_t *dev = &ahci_devices[ahci_device_count];
            memset(dev, 0, sizeof(*dev));
            dev->port        = (uint8_t)ahci_port_count;
            dev->reserved    = 1;
            dev->type        = AHCI_DEV_SATAPI;
            dev->sector_size = 2048;
            dev->size        = 0;
            ahci_write32(port->port_mmio, PORT_CMD, ahci_read32(port->port_mmio, PORT_CMD) | PORT_CMD_ATAPI);
            dbg_puts("ahci: port ");
            dbg_puts_dec((uint32_t)i);
            dbg_puts(": SATAPI optical device detected\n");
            ahci_device_count++;
            ahci_port_count++;
            continue;
        }

        /* SATA device: do IDENTIFY */
        ahci_device_t *dev = &ahci_devices[ahci_device_count];
        memset(dev, 0, sizeof(*dev));
        dev->port = (uint8_t)ahci_port_count;

        if (ahci_port_identify(port, dev) != 0) {
            dbg_puts("ahci: port ");
            dbg_puts_dec((uint32_t)i);
            dbg_puts(" identify failed\n");
            ahci_port_stop(port);
            continue;
        }

        ahci_device_count++;
        ahci_port_count++;

        dbg_puts("ahci: port ");
        dbg_puts_dec((uint32_t)i);
        dbg_puts(": SATA drive, ");
        dbg_puts_dec((uint32_t)(dev->size * ATA_SECTOR / 1048576));
        dbg_puts(" MiB, model \"");
        dbg_puts(dev->model);
        dbg_puts("\"\n");
    }
    if (ahci_device_count > 0) {
        dbg_puts("ahci: ");
        dbg_puts_dec((uint32_t)ahci_device_count);
        dbg_puts(" device(s) found\n");
    }

    ahci_satapi_init();
}
