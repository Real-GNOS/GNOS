/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ata_block.c — /dev nodes, partition tables and the probe loop. (GPLv2)
 *
 * ide.c knows how to move a sector over the legacy PIO channels, atapi.c
 * how to talk SCSI to a CD-ROM on one of them, ahci.c how to do the same
 * over a SATA (AHCI) controller, and satapi.c the SCSI half of that.  None
 * of them should know that GNOS exposes disks as files.  This file is the
 * glue: it probes both backends, publishes the whole disks and their
 * MBR/GPT partitions, and provides the installer's early boot read.
 * Uinxed has no counterpart because it has no /dev block nodes, so this
 * stays a GNOS-specific file rather than being squeezed into a driver.
 *
 * The one difference the read/write path has to absorb: the PIO side moves
 * one 512-byte sector per call, while the AHCI side moves runs of sectors
 * through a per-port bounce buffer.  bdev_read_sectors and
 * bdev_write_sectors hide that behind one sector-oriented API.
 */
#include <stddef.h>
#include <stdint.h>

#include "ata.h"
#include "ide.h"
#include "atapi.h"
#include "ahci.h"
#include "satapi.h"
#include "vfs.h"
#include "subsys.h"
#include "kstring.h"
#include "debugcon.h"

/* ---- devices ----------------------------------------------------------- */
#define MAX_PATA_DISKS 4 /* two channels, master and slave on each */
#define MAX_DISKS      (MAX_PATA_DISKS + AHCI_MAX_DEVICES)
#define MAX_PARTS      16 /* MBR has four; GPT commonly has 128, we take the first sixteen */

/*
 * What a /dev node points at.  The whole disk and each of its partitions use
 * the same structure and the same operations; a partition is just a window
 * with a non-zero start and a shorter length.  Doing it this way means the
 * read/write path has no idea partitions exist, which is exactly the amount
 * it needs to know.
 *
 * The backend is one of two: a PIO channel (pata set) or an entry of the
 * AHCI device table (ahci_idx), which covers both the SATA disks and the
 * CD-ROMs satapi.c drives.
 */
typedef struct {
    const ata_disk_t *pata;     /* PIO channel state, NULL when AHCI */
    int               ahci_idx; /* index into ahci_devices[], -1 when PATA */
    uint64_t          lba0;     /* first sector of the window   */
    uint64_t          nsect;    /* window length, in sectors    */
    uint32_t          secsize;  /* bytes per sector: 512 ATA, 2048 ATAPI */
    char              name[8];  /* "sda", "sda1"                */
    char              model[41];
    uint8_t           is_part;
    uint8_t           used;
} ata_bdev_t;

static ata_disk_t g_pata_disks[MAX_PATA_DISKS];
static int        g_ndisks;

/* Slot 0..MAX_DISKS-1 are the whole disks; the rest are partition windows,
 * MAX_PARTS per disk, indexed so a re-read can find and replace them. */
static ata_bdev_t g_bdevs[MAX_DISKS + MAX_DISKS * MAX_PARTS];

static ata_bdev_t *part_slot(int disk, int idx)
{
    return &g_bdevs[MAX_DISKS + disk * MAX_PARTS + idx];
}

/* ---- byte-addressed block I/O ------------------------------------------ */
/*
 * Byte-addressed read and write on top of a sector-addressed device.  The
 * head and tail of a transfer are almost never sector-aligned, so both are
 * done through a bounce buffer: read the sector, patch the interesting part,
 * write it back.  That read-modify-write is what makes `echo x > /dev/sda`
 * change one byte instead of destroying 511 of its neighbours.
 */
static int32_t clamp(const ata_bdev_t *b, uint64_t off, uint32_t len, uint64_t *end)
{
    uint64_t cap = b->nsect * b->secsize;
    if (off >= cap)
        return 0; /* past the end reads as EOF */
    if (off + len > cap)
        len = (uint32_t)(cap - off);
    *end = off + len;
    return (int32_t)len;
}

/*
 * Move nsect sectors to or from the disk, whatever backend owns it.  The
 * PIO side takes one 512-byte sector per call (a legacy interface) or a
 * packet run for a CD; the AHCI side takes runs of up to 255 per call, its
 * sector count field being a byte.  Returns 0, or a negative errno.
 */
static int bdev_read_sectors(const ata_bdev_t *b, uint64_t lba, uint32_t nsect, uint8_t *out)
{
    if (!nsect)
        return 0;

    if (b->pata) {
        if (b->secsize == ATA_SECTOR) {
            for (uint32_t i = 0; i < nsect; i++)
                if (ata_read_sector(b->pata, lba + i, out + (size_t)i * ATA_SECTOR) < 0)
                    return -E_IO;
            return 0;
        }
        return atapi_read(b->pata, lba, nsect, out) < 0 ? -E_IO : 0;
    }

    uint32_t left = nsect;
    while (left) {
        uint8_t now = left > 255 ? 255 : (uint8_t)left;
        int     ret = (b->secsize == ATA_SECTOR)
                          ? ahci_read_sectors((uint8_t)b->ahci_idx, now, lba, out)
                          : ahci_satapi_read_sectors((uint8_t)b->ahci_idx, now, lba, out);
        if (ret < 0)
            return ret;
        out += (size_t)now * b->secsize;
        lba += now;
        left -= now;
    }
    return 0;
}

static int bdev_write_sectors(const ata_bdev_t *b, uint64_t lba, uint32_t nsect, const uint8_t *in)
{
    if (!nsect)
        return 0;

    if (b->pata) {
        for (uint32_t i = 0; i < nsect; i++)
            if (ata_write_sector(b->pata, lba + i, in + (size_t)i * ATA_SECTOR) < 0)
                return -E_IO;
        return 0;
    }

    if (b->secsize != ATA_SECTOR)
        return -E_NOSYS; /* ATAPI publishes read-only */

    uint32_t left = nsect;
    while (left) {
        uint8_t now = left > 255 ? 255 : (uint8_t)left;
        int     ret = ahci_write_sectors((uint8_t)b->ahci_idx, now, lba, in);
        if (ret < 0)
            return ret;
        in += (size_t)now * ATA_SECTOR;
        lba += now;
        left -= now;
    }

    /* The data sits in the drive's cache until flushed; the PIO path flushes
     * after every sector, so do the same to keep the durability story
     * identical on both backends. */
    return ahci_flush_cache((uint8_t)b->ahci_idx);
}

static int32_t bdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used || !b->secsize)
        return -E_IO;

    uint64_t end;
    int32_t  want = clamp(b, off, len, &end);
    if (want <= 0)
        return want;

    uint8_t *out = (uint8_t *)buf;
    uint64_t pos = off;
    uint8_t  sec[2048]; /* one sector of either kind */

    while (pos < end) {
        uint64_t lba   = b->lba0 + pos / b->secsize;
        uint32_t skip  = (uint32_t)(pos % b->secsize);
        uint32_t chunk = b->secsize - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);

        if (skip == 0 && chunk == b->secsize) {
            /* Whole sector(s): let the backend use its multi-sector path,
             * capped at what one packet or command carries comfortably. */
            uint32_t ns = (uint32_t)((end - pos) / b->secsize);
            if (ns > 32)
                ns = 32;
            if (bdev_read_sectors(b, lba, ns, out) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
            out += ns * b->secsize;
            pos += ns * b->secsize;
            continue;
        }

        if (bdev_read_sectors(b, lba, 1, sec) < 0)
            return (pos > off) ? (int32_t)(pos - off) : -E_IO;
        memcpy(out, sec + skip, chunk);
        out += chunk;
        pos += chunk;
    }
    return (int32_t)(end - off);
}

static int32_t bdev_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used || !b->secsize)
        return -E_IO;

    uint64_t end;
    int32_t  want = clamp(b, off, len, &end);
    if (want <= 0)
        return -E_NOSPC; /* a write past the end is ENOSPC */

    const uint8_t *in  = (const uint8_t *)buf;
    uint64_t       pos = off;
    uint8_t        sec[2048];

    while (pos < end) {
        uint64_t lba   = b->lba0 + pos / b->secsize;
        uint32_t skip  = (uint32_t)(pos % b->secsize);
        uint32_t chunk = b->secsize - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);

        if (chunk == b->secsize) {
            if (bdev_write_sectors(b, lba, 1, in) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
        } else {
            if (bdev_read_sectors(b, lba, 1, sec) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
            memcpy(sec + skip, in, chunk);
            if (bdev_write_sectors(b, lba, 1, sec) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
        }
        in += chunk;
        pos += chunk;
    }
    return want;
}

/* The block ioctls Linux defines; a partitioner or mkfs asks for these before
 * it will touch anything. */
#define BLKRRPART    0x125F
#define BLKGETSIZE   0x1260
#define BLKFLSBUF    0x1261
#define BLKSSZGET    0x1268
#define BLKGETSIZE64 0x80081272

static int ata_scan_partitions(int disk);
static int ata_scan_gpt(int disk);

static int32_t bdev_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;

    switch (cmd) {
    case BLKGETSIZE64:
        if (!arg)
            return -E_FAULT;
        *(uint64_t *)(uintptr_t)arg = b->nsect * b->secsize;
        return 0;

    case BLKGETSIZE: /* the old one: a count of sectors */
        if (!arg)
            return -E_FAULT;
        *(unsigned long *)(uintptr_t)arg = (unsigned long)b->nsect;
        return 0;

    case BLKSSZGET:
        if (!arg)
            return -E_FAULT;
        *(int *)(uintptr_t)arg = b->secsize;
        return 0;

    case BLKFLSBUF:
        return 0; /* nothing is cached to flush */

    case BLKRRPART:
        /* Re-reading the table of a partition makes no sense; only the whole
         * disk has one.  This is what Linux answers too. */
        if (b->is_part)
            return -E_INVAL;
        return ata_scan_partitions((int)(b - g_bdevs));

    default:
        return -E_NOTTY;
    }
}

/* ATAPI CD-ROMs publish read-only: the same byte-addressed read, with the
 * write and ioctl paths left out of the ops so a stray write gets the
 * kernel's default refusal instead of a driver bug. */
static const vfs_ops_t g_sr_ops = {
    .read = bdev_read,
};

static const vfs_ops_t g_bdev_ops = {
    .read  = bdev_read,
    .write = bdev_write,
    .ioctl = bdev_ioctl,
    .mmap  = 0,
};

/* ---- the partition table ----------------------------------------------- */
/*
 * The partition table, MBR first.  A classic MBR: 446 bytes of boot code,
 * four 16-byte partition entries, and the two-byte 0xAA55 signature.  Each
 * entry is
 *
 *   0  status (0x80 = bootable)
 *   4  type   (0x00 = unused, 0x83 = Linux, 0xEE = protective GPT)
 *   8  first LBA, little-endian
 *  12  sector count, little-endian
 *
 * A GPT disk is detected through its protective MBR entry (type 0xEE):
 * the real table then lives in the GPT header at LBA 1 and the entry array
 * it points to.  Only the type GUID is inspected (all-zero = empty slot);
 * CRC32 checking is skipped, matching the "small and trusting" style of the
 * rest of this kernel.  Both table kinds are defined in 512-byte sectors,
 * so this whole section only ever runs on ATA disks, never on CD-ROMs.
 */
#define MBR_SIG_OFF     510
#define MBR_PART_OFF    446
#define GPT_SIG_OFF     0  /* "EFI PART" at the top of the LBA-1 header */
#define GPT_ENTRY_LBA   72 /* offset of the partition-entry array LBA */
#define GPT_ENTRY_NUM   80 /* number of entries */
#define GPT_ENTRY_SZ    84 /* size of one entry, always >= 128 */
#define GPT_ENTRY_MIN   128
#define GPT_TYPE_GUID   0    /* 16-byte type GUID at the top of an entry */
#define GPT_ENTRY_FIRST 32   /* first LBA of the partition */
#define GPT_ENTRY_LAST  40   /* last LBA of the partition */
#define GPT_PROTECTIVE  0xEE /* MBR type announcing a GPT disk */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void part_name(char *dst, const char *disk, int idx)
{
    int i = 0;
    while (disk[i] && i < 6) {
        dst[i] = disk[i];
        i++;
    }
    if (idx < 9)
        dst[i++] = (char)('1' + idx);
    else {
        dst[i++] = (char)('1' + idx / 10);
        dst[i++] = (char)('0' + idx % 10);
    }
    dst[i] = 0;
}

/*
 * Read the first `want` bytes of GPT entry `i`.  The UEFI spec allows any
 * entry size and the array starts at an arbitrary LBA, so an entry can
 * straddle a sector boundary; read the second sector only when it does
 * instead of assuming 128-byte entries line up with 512-byte sectors.
 */
static int gpt_read_entry(const ata_bdev_t *b, uint64_t entry_lba, uint32_t entry_sz, uint32_t i,
                          uint8_t *out, uint32_t want)
{
    uint64_t byte = entry_lba * ATA_SECTOR + (uint64_t)i * entry_sz;
    uint64_t lba  = b->lba0 + byte / ATA_SECTOR;
    uint32_t off  = (uint32_t)(byte % ATA_SECTOR);
    uint8_t  sec[ATA_SECTOR];
    uint32_t first;

    if (want > ATA_SECTOR * 2)
        return -1;

    if (bdev_read_sectors(b, lba, 1, sec) < 0)
        return -1;

    first = want;
    if (off + first > ATA_SECTOR)
        first = ATA_SECTOR - off;
    memcpy(out, sec + off, first);

    if (first < want) {
        if (bdev_read_sectors(b, lba + 1, 1, sec) < 0)
            return -1;
        memcpy(out + first, sec, want - first);
    }
    return 0;
}

/*
 * (Re-)publish /dev/sdaN for one disk.  Every existing partition node for
 * that disk is withdrawn first: after an installer rewrites the table, a
 * leftover window pointing into what is now somebody else's partition is a
 * corruption waiting to happen.
 */
static int ata_scan_partitions(int disk)
{
    if (disk < 0 || disk >= g_ndisks)
        return -E_INVAL;

    ata_bdev_t *whole = &g_bdevs[disk];

    for (int i = 0; i < MAX_PARTS; i++) {
        ata_bdev_t *p = part_slot(disk, i);
        if (p->used) {
            vfs_unregister_dev(p->name);
            p->used = 0;
        }
    }

    uint8_t mbr[ATA_SECTOR];
    if (bdev_read_sectors(whole, whole->lba0, 1, mbr) < 0)
        return -E_IO;
    if (mbr[MBR_SIG_OFF] != 0x55 || mbr[MBR_SIG_OFF + 1] != 0xAA)
        return 0; /* unpartitioned; not an error */

    /* A protective MBR entry (type 0xEE) means the real table is GPT.  It
     * can be in any of the four slots, not just the first. */
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + MBR_PART_OFF + i * 16;
        if (e[4] == GPT_PROTECTIVE && rd32(e + 12) != 0)
            return ata_scan_gpt(disk);
    }

    int found = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e     = mbr + MBR_PART_OFF + i * 16;
        uint8_t        type  = e[4];
        uint32_t       start = rd32(e + 8);
        uint32_t       count = rd32(e + 12);

        if (!type || !count)
            continue;
        /* An entry that runs off the end of the disk is a corrupt table, not
         * a device: publishing it would let a write wander past the last
         * sector and fail one I/O at a time. */
        if (start >= whole->nsect || (uint64_t)start + count > whole->nsect)
            continue;

        ata_bdev_t *p = part_slot(disk, i);
        p->pata       = whole->pata; /* a partition inherits its disk's
                                      * backend, with a window on it */
        p->ahci_idx = whole->ahci_idx;
        p->lba0     = whole->lba0 + start;
        p->nsect    = count;
        p->secsize  = whole->secsize;
        p->is_part  = 1;
        p->used     = 1;
        part_name(p->name, whole->name, i);

        if (vfs_register_blkdev(p->name, &g_bdev_ops, p, p->nsect * p->secsize) != 0) {
            p->used = 0;
            continue;
        }
        found++;

        dbg_puts("ATA:   /dev/");
        dbg_puts(p->name);
        dbg_puts(" type=0x");
        dbg_puts_hexn(type, 2);
        dbg_puts(" start=");
        dbg_puts_dec(start);
        dbg_puts(" sectors=");
        dbg_puts_dec(count);
        dbg_puts("\n");
    }
    return found;
}

/*
 * Publish the partitions a GPT table declares.  The header at LBA 1 carries
 * the "EFI PART" signature and the location of the entry array; each entry
 * is at least 128 bytes of which we use the type GUID (all zero = unused
 * slot) and the first/last LBA.
 */
static int ata_scan_gpt(int disk)
{
    ata_bdev_t *whole = &g_bdevs[disk];

    uint8_t hdr[ATA_SECTOR];
    if (bdev_read_sectors(whole, whole->lba0 + 1, 1, hdr) < 0)
        return -E_IO;
    if (memcmp(hdr + GPT_SIG_OFF, "EFI PART", 8) != 0)
        return 0; /* protective MBR but no GPT: ignore */

    uint64_t entry_lba = rd64(hdr + GPT_ENTRY_LBA);
    uint32_t nentries  = rd32(hdr + GPT_ENTRY_NUM);
    uint32_t entry_sz  = rd32(hdr + GPT_ENTRY_SZ);
    if (entry_lba == 0 || nentries == 0 || entry_sz < GPT_ENTRY_MIN)
        return 0;
    if (nentries > MAX_PARTS)
        nentries = MAX_PARTS;

    int found = 0;
    for (uint32_t i = 0; i < nentries; i++) {
        /* 48 bytes cover the type GUID and the first/last LBA fields. */
        uint8_t ent[48];
        if (gpt_read_entry(whole, entry_lba, entry_sz, i, ent, sizeof ent) < 0)
            continue;

        const uint8_t *guid  = ent + GPT_TYPE_GUID;
        int            empty = 1;
        for (int g = 0; g < 16; g++)
            if (guid[g]) {
                empty = 0;
                break;
            }
        if (empty)
            continue;

        uint64_t first = rd64(ent + GPT_ENTRY_FIRST);
        uint64_t last  = rd64(ent + GPT_ENTRY_LAST);
        if (!first || last < first)
            continue;
        if (first >= whole->nsect || last >= whole->nsect)
            continue;

        uint64_t    count = last - first + 1;
        ata_bdev_t *p     = part_slot(disk, i);
        p->pata           = whole->pata; /* a partition inherits its
                                          * disk's backend, with a
                                          * window on it */
        p->ahci_idx = whole->ahci_idx;
        p->lba0     = whole->lba0 + first;
        p->nsect    = count;
        p->secsize  = whole->secsize;
        p->is_part  = 1;
        p->used     = 1;
        part_name(p->name, whole->name, i);

        if (vfs_register_blkdev(p->name, &g_bdev_ops, p, p->nsect * p->secsize) != 0) {
            p->used = 0;
            continue;
        }
        found++;

        dbg_puts("ATA:   /dev/");
        dbg_puts(p->name);
        dbg_puts(" gpt start=");
        dbg_puts_dec((uint32_t)first);
        dbg_puts(" sectors=");
        dbg_puts_dec((uint32_t)count);
        dbg_puts("\n");
    }
    return found;
}

/* ---- disk-root boot ---------------------------------------------------- */
/* Read the whole *first used partition* of disk 0 into `dst`, which must be
 * `cap` bytes.  Returns the byte count, or 0 when there is no disk or no
 * usable partition.  This is the installer's boot path: when no initrd came
 * in RAM, the root filesystem is whatever the installer wrote to the disk,
 * and it has to be picked up whole here, before paging has anything
 * user-visible to attach it to. */
int ata_read_boot_partition(uint8_t *dst, uint32_t cap)
{
    if (!g_ndisks)
        return 0;

    ata_bdev_t *p = NULL;
    for (int i = 0; i < MAX_PARTS; i++) {
        if (!part_slot(0, i)->used)
            continue;
        p = part_slot(0, i);
        break;
    }
    if (!p)
        return 0;

    uint64_t total = p->nsect * p->secsize;
    if (total > cap)
        return 0;

    for (uint64_t i = 0; i < p->nsect;) {
        uint32_t now = (uint32_t)(p->nsect - i);
        if (now > 255)
            now = 255;
        if (bdev_read_sectors(p, p->lba0 + i, now, dst + (size_t)i * p->secsize) < 0)
            return 0;
        i += now;
    }
    return (int)total;
}

/* ---- probe ------------------------------------------------------------- */
/*
 * The PIO channels are fixed ports, present on every x86 machine whether or
 * not anything is plugged in.  The AHCI controller, when the chipset has
 * one, comes from PCI and can carry several ports of its own.  Both backends
 * land in the same g_bdevs table, in probe order, so the publish pass below
 * can name the disks sda, sdb, ... and the CD-ROMs sr0, sr1, ... without
 * knowing which backend they came from.
 */
static const struct {
    uint16_t io, ctrl;
} g_channels[] = {
    {0x1F0, 0x3F6},
    {0x170, 0x376},
};

static void probe_pata(void)
{
    for (unsigned c = 0; c < sizeof g_channels / sizeof g_channels[0]; c++) {
        for (int s = 0; s < 2; s++) {
            if (g_ndisks >= MAX_PATA_DISKS)
                break;

            ata_disk_t probe;
            memset(&probe, 0, sizeof probe);
            probe.io    = g_channels[c].io;
            probe.ctrl  = g_channels[c].ctrl;
            probe.slave = (uint8_t)s;

            if (!ata_identify(&probe))
                continue;

            int         idx = g_ndisks++;
            ata_disk_t *d   = &g_pata_disks[idx];
            *d              = probe;

            ata_bdev_t *b = &g_bdevs[idx];
            b->pata       = d;
            b->ahci_idx   = -1;
            b->lba0       = 0;
            b->nsect      = d->sectors;
            b->secsize    = d->secsize;
            b->is_part    = 0;
            b->used       = 1;
            memcpy(b->model, d->model, sizeof b->model);
        }
    }
}

static void probe_ahci(void)
{
    ahci_init();

    for (int i = 0; i < ahci_device_count && g_ndisks < MAX_DISKS; i++) {
        ahci_device_t *a = &ahci_devices[i];

        int         idx = g_ndisks++;
        ata_bdev_t *b   = &g_bdevs[idx];
        b->pata         = NULL;
        b->ahci_idx     = i;
        b->lba0         = 0;
        b->nsect        = a->size;
        b->secsize      = a->sector_size;
        b->is_part      = 0;
        b->used         = 1;
        memcpy(b->model, a->model, sizeof b->model);
    }
}

/* ---- publish ----------------------------------------------------------- */
/*
 * (Re-)publish the whole disks and their partitions.  Running it twice
 * against the same VFS table is harmless (the scan withdraws and re-registers
 * its partition nodes, and a /dev name registered twice is a duplicate the
 * table would keep the first of); running it after vfs_init() is what the
 * disk-root boot needs, because the table it filled in before the VFS
 * existed was wiped.
 */
static void publish_disks(void)
{
    int sr = 0;

    for (int idx = 0; idx < g_ndisks; idx++) {
        ata_bdev_t *b = &g_bdevs[idx];
        if (!b->used)
            continue;

        /* Whole-disk name: sd for ATA disks, sr for CD-ROMs.  The letters
         * continue across backends, so a machine with one IDE disk and one
         * SATA disk sees sda and sdb. */
        if (b->secsize != ATA_SECTOR) {
            b->name[0] = 's';
            b->name[1] = 'r';
            b->name[2] = (char)('0' + sr++);
            b->name[3] = 0;
        } else {
            b->name[0] = 's';
            b->name[1] = 'd';
            b->name[2] = (char)('a' + idx);
            b->name[3] = 0;
        }

        const vfs_ops_t *ops = (b->secsize == ATA_SECTOR) ? &g_bdev_ops : &g_sr_ops;
        if (vfs_register_blkdev(b->name, ops, b, b->nsect * b->secsize) != 0) {
            b->used = 0;
            g_ndisks--;
            continue;
        }

        /* Each disk after the first gets its own registry entry, so
         * coldplug can assert on every node and not just /dev/sda.  The
         * find-first keeps a re-publish from registering the name twice. */
        if (idx > 0 && subsys_find(b->name) < 0)
            subsys_set_state(
                subsys_register(b->name, b->name, SUBSYS_CLASS_BLOCK, 8, (uint16_t)(idx * 16)),
                SUBSYS_STATE_LIVE);

        if (b->secsize != ATA_SECTOR) {
            dbg_puts("ATA: /dev/");
            dbg_puts(b->name);
            dbg_puts(" CD-ROM \"");
            dbg_puts(b->model);
            dbg_puts("\"\n");
            continue;
        }

        dbg_puts("ATA: /dev/");
        dbg_puts(b->name);
        dbg_puts(" ");
        dbg_puts_dec((uint32_t)(b->nsect * b->secsize / 1048576));
        dbg_puts(" MiB, \"");
        dbg_puts(b->model);
        dbg_puts("\"\n");

        ata_scan_partitions(idx);
    }
}

int ata_init(void)
{
    /*
     * Probe once, publish every time.  The disks are there at boot or they
     * never are, and a second probe would re-identify live drives behind the
     * nodes already published -- and, in the disk-root build, smash the very
     * sectors the root filesystem was just copied out of.  The publish pass
     * has to run on every call, though: the disk-root boot calls this before
     * the VFS exists, and vfs_init() wipes the /dev table it filled in
     * there, so without it a disk-root system would come up with no
     * /dev/sda at all.
     */
    static int g_ata_inited;
    if (!g_ata_inited) {
        g_ata_inited = 1;
        probe_pata();
        probe_ahci();
    }

    int slot = subsys_find("ata");
    if (slot < 0)
        slot = subsys_register("ata", "sda", SUBSYS_CLASS_BLOCK, 8, 0);

    publish_disks();

    if (!g_ndisks) {
        dbg_puts("ATA: no disks\n");
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        return 0;
    }
    subsys_set_state(slot, SUBSYS_STATE_LIVE);
    return g_ndisks;
}
