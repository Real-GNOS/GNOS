/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ata_block.c — /dev nodes, partition tables and the probe loop. (GPLv2)
 *
 * ide.c knows how to move a sector and atapi.c knows how to talk SCSI to a
 * CD-ROM; neither should know that GNOS exposes disks as files.  This file
 * is the glue: it probes the two legacy channels, publishes the whole disks
 * and their MBR/GPT partitions, and provides the installer's early boot
 * read.  Uinxed has no counterpart because it has no /dev block nodes, so
 * this stays a GNOS-specific third file rather than being squeezed into the
 * PATA driver.
 */
#include <stdint.h>

#include "ata.h"
#include "ide.h"
#include "atapi.h"
#include "vfs.h"
#include "subsys.h"
#include "kstring.h"
#include "debugcon.h"

/* ---- devices ----------------------------------------------------------- */
#define MAX_DISKS 4  /* two channels, master and slave on each */
#define MAX_PARTS 16 /* MBR has four; GPT commonly has 128, we take the first sixteen */

/*
 * What a /dev node points at.  The whole disk and each of its partitions use
 * the same structure and the same operations; a partition is just a window
 * with a non-zero start and a shorter length.  Doing it this way means the
 * read/write path has no idea partitions exist, which is exactly the amount
 * it needs to know.
 */
typedef struct {
    ata_disk_t *disk;
    uint64_t    lba0;    /* first sector of the window   */
    uint64_t    nsect;   /* window length, in sectors    */
    char        name[8]; /* "sda", "sda1"                */
    uint8_t     is_part;
    uint8_t     used;
} ata_bdev_t;

static ata_disk_t g_disks[MAX_DISKS];
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
static int32_t clamp(const ata_bdev_t *b, uint64_t off, uint32_t len,
                     uint64_t *end)
{
    uint64_t cap = b->nsect * ATA_SECTOR;
    if (off >= cap)
        return 0; /* past the end reads as EOF */
    if (off + len > cap)
        len = (uint32_t)(cap - off);
    *end = off + len;
    return (int32_t)len;
}

static int32_t bdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;

    uint64_t end;
    int32_t  want = clamp(b, off, len, &end);
    if (want <= 0)
        return want;

    uint8_t *out = (uint8_t *)buf;
    uint8_t  sec[ATA_SECTOR];
    uint64_t pos = off;

    while (pos < end) {
        uint64_t lba   = b->lba0 + pos / ATA_SECTOR;
        uint32_t skip  = (uint32_t)(pos % ATA_SECTOR);
        uint32_t chunk = ATA_SECTOR - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);

        if (ata_read_sector(b->disk, lba, sec) < 0)
            return (pos > off) ? (int32_t)(pos - off) : -E_IO;
        memcpy(out, sec + skip, chunk);

        out += chunk;
        pos += chunk;
    }
    return want;
}

static int32_t bdev_write(vfs_node_t *n, uint64_t off, const void *buf,
                          uint32_t len)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;

    uint64_t end;
    int32_t  want = clamp(b, off, len, &end);
    if (want <= 0)
        return want == 0 ? -E_NOSPC : want; /* a write past the end is ENOSPC */

    const uint8_t *in = (const uint8_t *)buf;
    uint8_t        sec[ATA_SECTOR];
    uint64_t       pos = off;

    while (pos < end) {
        uint64_t lba   = b->lba0 + pos / ATA_SECTOR;
        uint32_t skip  = (uint32_t)(pos % ATA_SECTOR);
        uint32_t chunk = ATA_SECTOR - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);

        if (chunk == ATA_SECTOR) {
            memcpy(sec, in, ATA_SECTOR); /* whole sector: no read needed */
        } else {
            if (ata_read_sector(b->disk, lba, sec) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
            memcpy(sec + skip, in, chunk);
        }
        if (ata_write_sector(b->disk, lba, sec) < 0)
            return (pos > off) ? (int32_t)(pos - off) : -E_IO;

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
        *(uint64_t *)(uintptr_t)arg = b->nsect * ATA_SECTOR;
        return 0;

    case BLKGETSIZE: /* the old one: a count of sectors */
        if (!arg)
            return -E_FAULT;
        *(unsigned long *)(uintptr_t)arg = (unsigned long)b->nsect;
        return 0;

    case BLKSSZGET:
        if (!arg)
            return -E_FAULT;
        *(int *)(uintptr_t)arg = ATA_SECTOR;
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

/* ATAPI CD-ROM devices: read-only, 2048-byte sectors, no partition table.
 * Byte addressing is implemented the same way as for disks so a caller that
 * reads 512-byte blocks (dd, a bootloader) gets data instead of EINVAL. */
static int32_t sr_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used || !b->disk->secsize)
        return -E_IO;

    uint32_t secsize = b->disk->secsize;
    uint64_t cap     = b->nsect * secsize;
    uint64_t end     = off + len;

    if (off >= cap)
        return 0;
    if (end > cap)
        end = cap;

    uint8_t *out = (uint8_t *)buf;
    uint64_t pos = off;

    while (pos < end) {
        uint64_t lba   = b->lba0 + pos / secsize;
        uint32_t skip  = (uint32_t)(pos % secsize);
        uint32_t chunk = secsize - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);

        if (chunk == secsize) {
            /* Whole sector(s): let atapi_read use its multi-sector path,
             * capped at the 32-sector packet limit. */
            uint32_t ns = (uint32_t)((end - pos) / secsize);
            if (ns > 32)
                ns = 32;
            if (atapi_read(b->disk, lba, ns, out) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
            out += ns * secsize;
            pos += ns * secsize;
        } else {
            uint8_t sec[2048];
            if (atapi_read(b->disk, lba, 1, sec) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
            memcpy(out, sec + skip, chunk);
            out += chunk;
            pos += chunk;
        }
    }
    return (int32_t)(end - off);
}

static const vfs_ops_t g_sr_ops = {
    .read = sr_read,
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
 * rest of this kernel.
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
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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
static int gpt_read_entry(const ata_disk_t *d, uint64_t disk_lba0,
                          uint64_t entry_lba, uint32_t entry_sz, uint32_t i,
                          uint8_t *out, uint32_t want)
{
    uint64_t byte = entry_lba * ATA_SECTOR + (uint64_t)i * entry_sz;
    uint64_t lba  = disk_lba0 + byte / ATA_SECTOR;
    uint32_t off  = (uint32_t)(byte % ATA_SECTOR);
    uint8_t  sec[ATA_SECTOR];
    uint32_t first;

    if (off >= ATA_SECTOR)
        return -1;
    if (want > ATA_SECTOR * 2)
        return -1;

    if (ata_read_sector(d, lba, sec) < 0)
        return -1;

    first = want;
    if (off + first > ATA_SECTOR)
        first = ATA_SECTOR - off;
    memcpy(out, sec + off, first);

    if (first < want) {
        if (ata_read_sector(d, lba + 1, sec) < 0)
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
    if (ata_read_sector(whole->disk, whole->lba0, mbr) < 0)
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
        p->disk       = whole->disk;
        p->lba0       = whole->lba0 + start;
        p->nsect      = count;
        p->is_part    = 1;
        p->used       = 1;
        part_name(p->name, whole->name, i);

        if (vfs_register_blkdev(p->name, &g_bdev_ops, p,
                                p->nsect * ATA_SECTOR) != 0) {
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
    if (ata_read_sector(whole->disk, whole->lba0 + 1, hdr) < 0)
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
        if (gpt_read_entry(whole->disk, whole->lba0, entry_lba, entry_sz, i,
                           ent, sizeof ent) < 0)
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
        p->disk           = whole->disk;
        p->lba0           = whole->lba0 + first;
        p->nsect          = count;
        p->is_part        = 1;
        p->used           = 1;
        part_name(p->name, whole->name, i);

        if (vfs_register_blkdev(p->name, &g_bdev_ops, p,
                                p->nsect * ATA_SECTOR) != 0) {
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

    uint64_t total = p->nsect * ATA_SECTOR;
    if (total > cap)
        return 0;

    for (uint64_t i = 0; i < p->nsect; i++)
        if (ata_read_sector(p->disk, p->lba0 + i, dst + i * ATA_SECTOR) < 0)
            return 0;
    return (int)total;
}

/* ---- probe ------------------------------------------------------------- */
static const struct {
    uint16_t io, ctrl;
} g_channels[] = {
    {0x1F0, 0x3F6},
    {0x170, 0x376},
};

int ata_init(void)
{
    /* Once is enough: the disks are there at boot or they never are, and a
     * second pass would re-identify live drives behind the nodes already
     * published -- and, in the disk-root build, smash the very sectors the
     * root filesystem was just copied out of. */
    static int g_ata_inited;
    if (g_ata_inited)
        return g_ndisks;
    g_ata_inited = 1;

    int slot = subsys_register("ata", "sda", SUBSYS_CLASS_BLOCK, 8, 0);

    for (unsigned c = 0; c < sizeof g_channels / sizeof g_channels[0]; c++) {
        for (int s = 0; s < 2; s++) {
            if (g_ndisks >= MAX_DISKS)
                break;

            ata_disk_t probe;
            memset(&probe, 0, sizeof probe);
            probe.io    = g_channels[c].io;
            probe.ctrl  = g_channels[c].ctrl;
            probe.slave = (uint8_t)s;

            if (!ata_identify(&probe))
                continue;

            int         idx = g_ndisks++;
            ata_disk_t *d   = &g_disks[idx];
            *d              = probe;

            ata_bdev_t *b = &g_bdevs[idx];
            b->disk       = d;
            b->lba0       = 0;
            b->nsect      = d->sectors;
            b->is_part    = 0;
            b->used       = 1;

            static int g_sr_count;
            if (d->atapi) {
                /* A PACKET device: publish it as a read-only 2048-byte
                 * sector CD-ROM (sr0, sr1, ...) with no partition table. */
                b->name[0] = 's';
                b->name[1] = 'r';
                b->name[2] = (char)('0' + g_sr_count++);
                b->name[3] = 0;
                if (vfs_register_blkdev(b->name, &g_sr_ops, b,
                                        b->nsect * d->secsize) != 0) {
                    b->used = 0;
                    g_ndisks--;
                    continue;
                }
                dbg_puts("ATA: /dev/");
                dbg_puts(b->name);
                dbg_puts(" CD-ROM \"");
                dbg_puts(d->model);
                dbg_puts("\"\n");
                continue;
            }

            b->name[0] = 's';
            b->name[1] = 'd';
            b->name[2] = (char)('a' + idx);
            b->name[3] = 0;

            if (vfs_register_blkdev(b->name, &g_bdev_ops, b,
                                    b->nsect * ATA_SECTOR) != 0) {
                b->used = 0;
                g_ndisks--;
                continue;
            }

            /* Each disk after the first gets its own registry entry, so
             * coldplug can assert on every node and not just /dev/sda. */
            if (idx > 0)
                subsys_set_state(
                    subsys_register(b->name, b->name, SUBSYS_CLASS_BLOCK, 8,
                                    (uint16_t)(idx * 16)),
                    SUBSYS_STATE_LIVE);

            dbg_puts("ATA: /dev/");
            dbg_puts(b->name);
            dbg_puts(" ");
            dbg_puts_dec((uint32_t)(d->sectors / 2048));
            dbg_puts(" MiB, lba");
            dbg_puts(d->lba48 ? "48" : "28");
            dbg_puts(", \"");
            dbg_puts(d->model);
            dbg_puts("\"\n");

            ata_scan_partitions(idx);
        }
    }

    if (!g_ndisks) {
        dbg_puts("ATA: no disks\n");
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        return 0;
    }
    subsys_set_state(slot, SUBSYS_STATE_LIVE);
    return g_ndisks;
}
