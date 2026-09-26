/*
 * usb_msc.c — USB mass-storage class over Bulk-Only Transport. (GPLv2)
 *
 * Sits on the xHCI bulk transfer primitives.  A device whose interface is
 * class 8 / subclass 6 / protocol 0x50 (BOT) is driven through the
 * standard CBW/CSW packet exchange: a 31-byte Command Block Wrapper holds
 * the SCSI command, and a 13-byte Command Status Wrapper reports its
 * outcome.  On top of that we speak the three SCSI commands a read-only
 * installer or a partitioner needs -- TEST UNIT READY, READ CAPACITY(10)
 * and READ(10) -- and publish the device as /dev/sdb through the same
 * block-device layer ATA uses.
 *
 * Write support is deliberately omitted: this kernel's boot story never
 * writes to a USB disk, and a bug in a mass-storage write is how you lose
 * data.  READ(10) is enough to mount a FAT32 or ext2 flash drive.
 */
#include <stdint.h>

#include "xhci.h"
#include "vfs.h"
#include "kstring.h"
#include "heap.h"
#include "timer.h"
#include "debugcon.h"
#include "subsys.h"

#define MSC_MAX_DEVICES 4
#define MSC_SECTOR      512

#define MSC_CBW_SIGNATURE 0x43425355u    /* USBC */
#define MSC_CSW_SIGNATURE 0x53425355u    /* USBS */
#define MSC_CBW_LENGTH    31
#define MSC_CSW_LENGTH    13

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY   0x25
#define SCSI_READ_10         0x28

typedef struct {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;          /* 0x80 = IN (device->host) */
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} __attribute__((packed)) msc_cbw_t;

typedef struct {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} __attribute__((packed)) msc_csw_t;

typedef struct {
    int      slot;
    int      ep_in;               /* bulk IN  endpoint address */
    int      ep_out;              /* bulk OUT endpoint address */
    uint16_t ep_in_packet;
    uint16_t ep_out_packet;
    uint32_t nblocks;             /* from READ CAPACITY */
    uint8_t  active;
} msc_dev_t;

static msc_dev_t g_msc[MSC_MAX_DEVICES];
static int g_msc_count;
static int g_msc_ready;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---- the CBW/CSW exchange ------------------------------------------------- */

static int msc_command(msc_dev_t *dev, const uint8_t *cdb, uint8_t cdb_len,
                       void *data, uint32_t data_len, int dir_in)
{
    if (!dev || !dev->active || !cdb || !cdb_len || cdb_len > 16)
        return -1;

    static uint32_t tag;
    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = MSC_CBW_SIGNATURE;
    cbw.dCBWTag = ++tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir_in ? 0x80 : 0x00;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    if (xhci_bulk_transfer(dev->slot, dev->ep_out, &cbw,
                           MSC_CBW_LENGTH) != MSC_CBW_LENGTH)
        return -1;

    uint32_t transferred = 0;
    if (data && data_len > 0) {
        int ret = dir_in
            ? xhci_bulk_transfer(dev->slot, dev->ep_in, data, data_len)
            : xhci_bulk_transfer(dev->slot, dev->ep_out, data, data_len);
        if (ret < 0 || (uint32_t)ret > data_len)
            return -1;
        transferred = (uint32_t)ret;
    }

    msc_csw_t csw;
    if (xhci_bulk_transfer(dev->slot, dev->ep_in, &csw,
                           MSC_CSW_LENGTH) != MSC_CSW_LENGTH)
        return -1;
    if (csw.dCSWSignature != MSC_CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag)
        return -1;
    if (csw.bCSWStatus != 0)
        return -1;
    if (transferred != data_len - csw.dCSWDataResidue)
        return -1;
    return (int)transferred;
}

/* ---- SCSI commands ---------------------------------------------------------- */

static int msc_read_capacity(msc_dev_t *dev)
{
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY;
    cdb[8] = 0x08;                       /* return 8 bytes */

    uint8_t resp[8];
    if (msc_command(dev, cdb, 10, resp, sizeof(resp), 1) < 0)
        return -1;
    dev->nblocks = rd32be(resp) + 1;     /* last LBA + 1 */
    return 0;
}

static int msc_read_blocks(msc_dev_t *dev, uint64_t lba, void *buf,
                           uint32_t nblocks)
{
    while (nblocks > 0) {
        uint32_t chunk = nblocks > 64 ? 64 : nblocks;
        uint8_t cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = SCSI_READ_10;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;

        if (msc_command(dev, cdb, 10, buf, chunk * MSC_SECTOR, 1) < 0)
            return -1;
        buf += chunk * MSC_SECTOR;
        lba += chunk;
        nblocks -= chunk;
    }
    return 0;
}

/* ---- the block-device layer --------------------------------------------------- */

typedef struct {
    msc_dev_t *dev;
} msc_bdev_t;

static msc_bdev_t g_msc_bdevs[MSC_MAX_DEVICES];

static int32_t msc_bdev_read(vfs_node_t *n, uint64_t off, void *buf,
                             uint32_t len)
{
    msc_bdev_t *b = (msc_bdev_t *)n->priv;
    if (!b || !b->dev || !b->dev->active)
        return -E_IO;

    uint64_t end = off + len;
    uint8_t *out = (uint8_t *)buf;
    uint8_t sec[MSC_SECTOR];
    uint64_t pos = off;

    while (pos < end) {
        uint64_t lba = pos / MSC_SECTOR;
        uint32_t skip = (uint32_t)(pos % MSC_SECTOR);
        uint32_t chunk = MSC_SECTOR - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);
        if (msc_read_blocks(b->dev, lba, sec, 1) < 0)
            return -E_IO;
        memcpy(out, sec + skip, chunk);
        out += chunk;
        pos += chunk;
    }
    return (int32_t)(end - off);
}

static int32_t msc_bdev_write(vfs_node_t *n, uint64_t off, const void *buf,
                              uint32_t len)
{
    (void)n;
    (void)off;
    (void)buf;
    (void)len;
    return -E_ROFS;                    /* deliberately read-only */
}

static int32_t msc_bdev_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    (void)n;
    (void)cmd;
    (void)arg;
    return -E_NOTTY;
}

static const vfs_ops_t g_msc_bdev_ops = {
    .read  = msc_bdev_read,
    .write = msc_bdev_write,
    .ioctl = msc_bdev_ioctl,
    .mmap  = 0,
};

/* ---- probe ---------------------------------------------------------------------- */

static int msc_probe_config(msc_dev_t *dev)
{
    int slot = dev->slot;

    uint8_t header[9];
    if (xhci_control_transfer(slot, 0x80, 6, 0x0200, 0,
                              header, sizeof(header)) < 0)
        return -1;
    uint16_t total = rd16(header + 2);
    if (total < sizeof(header) || total > 4096)
        return -1;

    uint8_t *cfg = kmalloc(total);
    if (!cfg)
        return -1;
    int ret = xhci_control_transfer(slot, 0x80, 6, 0x0200, 0, cfg, total);
    if (ret < (int)sizeof(header)) {
        kfree(cfg);
        return -1;
    }
    uint16_t got = ret < total ? (uint16_t)ret : total;

    int current_msc = 0;
    int ep_in = 0, ep_out = 0;
    uint16_t ep_in_packet = 0, ep_out_packet = 0;

    for (uint16_t off = 0; off + 2 <= got; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > got)
            break;

        if (type == 4 && len >= 9) {
            current_msc = cfg[off + 5] == 0x08 &&     /* class 8  */
                          cfg[off + 6] == 0x06 &&     /* subclass */
                          cfg[off + 7] == 0x50;       /* BOT      */
        } else if (type == 5 && len >= 7 && current_msc &&
                   (cfg[off + 3] & 0x03) == 0x02) {   /* bulk EP */
            uint8_t addr = cfg[off + 2];
            uint16_t packet = rd16(cfg + off + 4) & 0x07FFu;
            if (addr & 0x80) {
                ep_in = addr;
                ep_in_packet = packet;
            } else {
                ep_out = addr;
                ep_out_packet = packet;
            }
        }
        off += len;
    }
    kfree(cfg);

    if (!ep_in || !ep_out)
        return -1;

    dev->ep_in = ep_in;
    dev->ep_out = ep_out;
    dev->ep_in_packet = ep_in_packet ? ep_in_packet : 512;
    dev->ep_out_packet = ep_out_packet ? ep_out_packet : 512;

    /* TUR: wait for the drive to be ready (spinning up). */
    for (int i = 0; i < 10; i++) {
        uint8_t tur[6];
        memset(tur, 0, sizeof(tur));
        tur[0] = SCSI_TEST_UNIT_READY;
        if (msc_command(dev, tur, 6, NULL, 0, 1) >= 0)
            break;
        timer_delay_ms(100);
    }
    if (msc_read_capacity(dev) < 0)
        return -1;
    return 0;
}

void usb_msc_init(void)
{
    int count = xhci_device_count();
    for (int i = 0; i < count && g_msc_count < MSC_MAX_DEVICES; i++) {
        int slot = xhci_device_slot(i);
        if (slot < 0)
            continue;

        msc_dev_t *dev = &g_msc[g_msc_count];
        memset(dev, 0, sizeof(*dev));
        dev->slot = slot;
        if (msc_probe_config(dev) < 0)
            continue;

        dev->active = 1;
        msc_bdev_t *b = &g_msc_bdevs[g_msc_count];
        b->dev = dev;

        char name[8];
        memcpy(name, "sdb", 4);       /* ATA owns sda; USB disks come next */
        if (vfs_register_blkdev(name, &g_msc_bdev_ops, b,
                                dev->nblocks * MSC_SECTOR) != 0) {
            dev->active = 0;
            continue;
        }

        dbg_puts("USBMS: /dev/");
        dbg_puts(name);
        dbg_puts(", ");
        dbg_puts_dec(dev->nblocks);
        dbg_puts(" sectors, slot ");
        dbg_puts_dec(slot);
        dbg_puts("\r\n");
        g_msc_count++;
    }
    g_msc_ready = 1;
}
