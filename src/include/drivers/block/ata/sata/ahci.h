/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ahci.h — SATA (AHCI) controllers: HBA registers and command structures. (GPLv2)
 *
 * Where the legacy PATA channels are eight fixed ISA ports shared by every
 * drive on the box, an AHCI controller is a PCI device whose registers are
 * memory mapped: the first page of its BAR holds the controller (HOST_CAP,
 * HOST_CTL, ...) and every further 0x80-byte page is one port, each port
 * driving one SATA cable.  A command is not a register write at all -- it is
 * a 20-byte register FIS plus a small command table written to RAM, and
 * pointing the port's command-issue register at the slot's header, after
 * which the port moves the data itself through its PRDT (a mini scatter/
 * gather list) and clears the slot when it is done.
 *
 * That has two consequences for this driver.  One: every transfer needs a
 * physically contiguous bounce buffer, because the PRDT entry advertises a
 * flat physical address range to the port -- a per-port buffer is allocated
 * at probe and every read and write shuttles through it.  Two: the command
 * memory (slot headers, FIS receive ring, command tables) is shared with
 * hardware, so it must come from the frame allocator and be addressed
 * through the direct map, never from the process heap.
 *
 * Like the PATA side, completion is polled (PORT_CI), not interrupt-driven;
 * the per-port lock below is what keeps two CPUs from handing the port two
 * commands that share one command table and one bounce buffer.
 *
 * The layout follows Uinxed's drivers/block/ata/sata/: ahci.c owns the HBA
 * and the ATA command set, satapi.c the SCSI packets a CD-ROM sends over the
 * same wires, and ata_block.c (in the parent directory) publishes the
 * devices this probe finds as /dev/sd* and /dev/sr*.
 */
#ifndef GNOS_ATA_SATA_AHCI_H
#define GNOS_ATA_SATA_AHCI_H

#include <stdint.h>

#include "ata.h"
#include "ata_cmds.h"
#include "smp.h"

/* What PORT_SIG says is sitting on the cable.  The last two bytes of every
 * value are 0x0101; the middle pair identifies the family. */
#define SATA_SIG_ATA   0x00000101
#define SATA_SIG_ATAPI 0xEB140101
#define SATA_SIG_SEMB  0xC33C0101
#define SATA_SIG_PM    0x96690101

/* Device classes for the table below.  SEMB/PM are not driven; the values
 * match Uinxed's so the two tables can be compared side by side. */
#define AHCI_DEV_NULL   0
#define AHCI_DEV_SATA   1
#define AHCI_DEV_SEMB   2
#define AHCI_DEV_PM     3
#define AHCI_DEV_SATAPI 4

/* Words of CFIS in a command header: 5 = the 20-byte register FIS. */
#define CFL_DWORDS 5

#define AHCI_MAX_PORTS   16
#define AHCI_MAX_DEVICES 16
#define AHCI_MAX_CMDS    32
#define AHCI_MAX_SG      8

/* ---- HBA register offsets (first page of the BAR) --------------------- */
#define HOST_CAP        0x00
#define HOST_CTL        0x04
#define HOST_IRQ_STAT   0x08
#define HOST_PORTS_IMPL 0x0c
#define HOST_VERSION    0x10
#define HOST_CAP2       0x24
#define HOST_BOHC       0x28

/* HOST_CTL bits. */
#define HOST_RESET   (1u << 0)
#define HOST_IRQ_EN  (1u << 1)
#define HOST_AHCI_EN (1u << 31)

/* HOST_CAP bits we read. */
#define HOST_CAP_64  (1u << 31)
#define HOST_CAP_NCQ (1u << 30)
#define HOST_CAP_CLO (1u << 24)

/* Port register offsets, relative to the port base (0x100 + port * 0x80). */
#define PORT_LST_ADDR    0x00
#define PORT_LST_ADDR_HI 0x04
#define PORT_FIS_ADDR    0x08
#define PORT_FIS_ADDR_HI 0x0c
#define PORT_IRQ_STAT    0x10
#define PORT_IRQ_MASK    0x14
#define PORT_CMD         0x18
#define PORT_TFDATA      0x20
#define PORT_SIG         0x24
#define PORT_SSTS        0x28
#define PORT_SCTL        0x2c
#define PORT_SERR        0x30
#define PORT_SACT        0x34
#define PORT_CI          0x38
#define PORT_SNTF        0x3c

/* PORT_CMD bits.  FRE (enable FIS receive) and ST (start) must both be set
 * before a command may be issued; CR and FR report that the engine came up. */
#define PORT_CMD_ST       (1u << 0)
#define PORT_CMD_SPIN_UP  (1u << 1)
#define PORT_CMD_POWER_ON (1u << 2)
#define PORT_CMD_FIS_RX   (1u << 4)
#define PORT_CMD_ATAPI    (1u << 24)
#define PORT_CMD_CR       (1u << 15)
#define PORT_CMD_FR       (1u << 14)
#define PORT_CMD_FRE      (1u << 4)

/* PORT_IRQ_STAT bits (read for errors even though nothing is interrupted). */
#define PORT_IRQ_TF_ERR      (1u << 30)
#define PORT_IRQ_HBUS_ERR    (1u << 29)
#define PORT_IRQ_IF_ERR      (1u << 27)
#define PORT_IRQ_CONNECT     (1u << 6)
#define PORT_IRQ_PHYRDY      (1u << 22)
#define PORT_IRQ_D2H_REG_FIS (1u << 0)

/* PORT_SSTS device-detect field (bits 3:0). */
#define HBA_PORT_DET_PRESENT 3

/* ---- FIS types --------------------------------------------------------- */
#define FIS_TYPE_REG_H2D   0x27
#define FIS_TYPE_REG_D2H   0x34
#define FIS_TYPE_DMA_ACT   0x39
#define FIS_TYPE_DMA_SETUP 0x41
#define FIS_TYPE_DATA      0x46
#define FIS_TYPE_BIST      0x58
#define FIS_TYPE_PIO_SETUP 0x5F
#define FIS_TYPE_DEV_BITS  0xA1

/* Register FIS, host to device: the "command" of an AHCI command. */
typedef struct {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t rsv0 : 3;
    uint8_t c : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

/* Register FIS, device to host: how the port answers. */
typedef struct {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t rsv0 : 2;
    uint8_t i : 1;
    uint8_t rsv1 : 1;
    uint8_t status;
    uint8_t error;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t rsv2;
    uint8_t countl;
    uint8_t counth;
    uint8_t rsv3[2];
    uint8_t rsv4[4];
} __attribute__((packed)) fis_reg_d2h_t;

/* PRDT entry: one physically contiguous chunk of a transfer. */
typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc : 22;
    uint32_t rsv1 : 9;
    uint32_t i : 1;
} __attribute__((packed)) hba_prdt_entry_t;

/* Command table, one per command slot, in RAM: CFIS, ATAPI CDB, PRDT. */
typedef struct {
    uint8_t          cfis[64];
    uint8_t          acmd[16];
    uint8_t          rsv[48];
    hba_prdt_entry_t prdt_entry[AHCI_MAX_SG];
} __attribute__((packed)) hba_cmd_tbl_t;

/* Command header, one per slot: the port reads these to learn about the
 * table.  cfl/a/w/p are the parts the driver sets; prdbc is written back by
 * the port with the number of bytes it actually moved. */
typedef struct {
    uint8_t           cfl : 5;
    uint8_t           a : 1;
    uint8_t           w : 1;
    uint8_t           p : 1;
    uint8_t           r : 1;
    uint8_t           b : 1;
    uint8_t           c : 1;
    uint8_t           rsv0 : 1;
    uint8_t           pmp : 4;
    uint16_t          prdtl;
    volatile uint32_t prdbc;
    uint32_t          ctba;
    uint32_t          ctbau;
    uint32_t          rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

/* The 256-byte FIS receive ring the port writes its answer FIS into.  The
 * polled driver never parses it -- error bits come from PORT_IRQ_STAT -- but
 * the ring must exist and be pointed at by PORT_FIS_ADDR, or the port
 * misbehaves. */
typedef struct {
    uint8_t bytes[256];
} hba_fis_ring_t;

/* Per-port state.  Everything the port touches lives in frames: the slot
 * header array (clb), the 256-byte FIS receive ring (fb) and this command
 * table (ct).  The port is single-command at a time here (one table, one
 * bounce buffer), which is all the polled design ever issues. */
typedef struct {
    volatile uint8_t *port_mmio;
    uint8_t           port_no;
    hba_cmd_header_t *cmd_list;
    hba_fis_ring_t   *fis;
    hba_cmd_tbl_t    *cmd_tbl;
    uint64_t          clb_phys;
    uint64_t          fb_phys;
    uint64_t          ct_phys;
    uint8_t          *dma_buf;
    uint64_t          dma_buf_phys;

    /*
     * Serialises command-slot allocation against the shared command table
     * and bounce buffer: two CPUs reading different files on the same disk
     * would otherwise interleave their PRDTs.  Completion is polled, so
     * holding it across a command deadlocks nothing.
     */
    spinlock_t lock;
} ahci_port_state_t;

/* One drive (or CD) the probe found.  size is the capacity in
 * sector_size-byte sectors: 512-byte LBA sectors for ATA, 2048 for ATAPI. */
typedef struct {
    uint8_t  reserved;
    uint8_t  type; /* AHCI_DEV_* */
    uint8_t  port; /* index into ahci_ports[] */
    uint64_t size;
    uint32_t sector_size;
    char     model[41];
} ahci_device_t;

/* The probe's results, and the port table the SATPApi half shares. */
extern ahci_device_t     ahci_devices[AHCI_MAX_DEVICES];
extern int               ahci_device_count;
extern volatile uint8_t *hba_mmio;
extern ahci_port_state_t ahci_ports[AHCI_MAX_PORTS];

/* Find the AHCI controller, bring up its ports and identify what is
 * plugged in.  Fills the tables above; safe to call more than once, the
 * first pass probes. */
void ahci_init(void);

/* Read/write one 32-bit register at offset reg from base.  The port code
 * keeps its own base in port_mmio, the HBA code uses hba_mmio. */
uint32_t ahci_read32(volatile uint8_t *base, uint32_t reg);
void     ahci_write32(volatile uint8_t *base, uint32_t reg, uint32_t val);

/* Free command slot on the port, or -E_BUSY when all HOST_CAP slots are
 * occupied. */
int ahci_find_slot(ahci_port_state_t *port);

/* Move numsects 512-byte sectors to or from the drive.  Returns 0 or a
 * negative errno. */
int ahci_read_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, void *buffer);
int ahci_write_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, const void *buffer);

/* Push the drive's write cache to platter. */
int ahci_flush_cache(uint8_t drive);

#endif /* GNOS_ATA_SATA_AHCI_H */
