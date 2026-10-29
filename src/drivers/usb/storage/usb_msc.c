/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usb_msc.c — USB mass-storage class over Bulk-Only Transport. (GPLv2)
 *
 * A class driver on the USB core: usb_add_device() dispatches to
 * usb_storage_probe() for every interface of class 8 / subclass 6 /
 * protocol 0x50 (BOT) after the device model has registered it.  The
 * standard CBW/CSW packet exchange moves SCSI commands over the bulk
 * endpoints through usb_bulk_msg(): a 31-byte Command Block Wrapper holds
 * the SCSI command, and a 13-byte Command Status Wrapper reports its
 * outcome.  On top of that we speak the three SCSI commands a read-only
 * installer or a partitioner needs -- TEST UNIT READY, READ CAPACITY(10)
 * and READ(10) -- and publish the device as /dev/sdb... through the same
 * block-device layer ATA uses.
 *
 * Write support is deliberately omitted: this kernel's boot story never
 * writes to a USB disk, and a bug in a mass-storage write is how you lose
 * data.  READ(10) is enough to mount a FAT32 or ext2 flash drive.
 */
#include <stdint.h>

#include "usb.h"
#include "../../scsi/scsi.h"
#include "vfs.h"
#include "kstring.h"
#include "timer.h"
#include "debugcon.h"

#define MSC_MAX_DEVICES 4
#define MSC_SECTOR      512

#define MSC_CBW_SIGNATURE 0x43425355u /* USBC */
#define MSC_CSW_SIGNATURE 0x53425355u /* USBS */
#define MSC_CBW_LENGTH    31
#define MSC_CSW_LENGTH    13

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY   0x25
#define SCSI_READ_10         0x28

typedef struct {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags; /* 0x80 = IN (device->host) */
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
    usb_endpoint_t *ep_in;   /* bulk IN  endpoint */
    usb_endpoint_t *ep_out;  /* bulk OUT endpoint */
    uint32_t        nblocks; /* from READ CAPACITY */
    uint8_t         active;
} msc_dev_t;

static msc_dev_t g_msc[MSC_MAX_DEVICES];
static int       g_msc_count;

/* ---- the CBW/CSW exchange ------------------------------------------------- */

static int msc_command(msc_dev_t *dev, const uint8_t *cdb, uint8_t cdb_len, void *data,
                       uint32_t data_len, int dir_in)
{
    if (!dev || !dev->active || !cdb || !cdb_len || cdb_len > 16)
        return -1;

    static uint32_t tag;
    msc_cbw_t       cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature          = MSC_CBW_SIGNATURE;
    cbw.dCBWTag                = ++tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags             = dir_in ? 0x80 : 0x00;
    cbw.bCBWCBLength           = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    size_t actual = 0;
    if (usb_bulk_msg(dev->ep_out, &cbw, MSC_CBW_LENGTH, &actual, USB_IO_TIMEOUT_MS) != 0 ||
        actual != MSC_CBW_LENGTH)
        return -1;

    uint32_t transferred = 0;
    if (data && data_len > 0) {
        usb_endpoint_t *ep = dir_in ? dev->ep_in : dev->ep_out;
        if (usb_bulk_msg(ep, data, data_len, &actual, USB_IO_TIMEOUT_MS) != 0 || actual > data_len)
            return -1;
        transferred = (uint32_t)actual;
    }

    msc_csw_t csw;
    if (usb_bulk_msg(dev->ep_in, &csw, MSC_CSW_LENGTH, &actual, USB_IO_TIMEOUT_MS) != 0 ||
        actual != MSC_CSW_LENGTH)
        return -1;
    if (csw.dCSWSignature != MSC_CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag)
        return -1;
    if (csw.bCSWStatus != 0)
        return -1;
    if (transferred != data_len - csw.dCSWDataResidue)
        return -1;
    return (int)transferred;
}

/* ---- the SCSI transport ---------------------------------------------------
 * The bus layer (src/drivers/scsi) builds the CDBs; this adapter only
 * moves them over the BOT protocol. */
static int msc_transport_execute(void *host, const uint8_t *cdb, uint8_t cdb_len, void *data,
                                 uint32_t data_len, int dir_in)
{
    return msc_command((msc_dev_t *)host, cdb, cdb_len, data, data_len, dir_in);
}

static scsi_transport_t g_msc_transport = {.execute = msc_transport_execute};
static scsi_device_t   *g_scsi; /* the scanned scsi device */

/* ---- the block-device layer --------------------------------------------------- */

typedef struct {
    msc_dev_t     *dev;
    scsi_device_t *sd; /* the bus-layer device this bdev serves */
} msc_bdev_t;

static msc_bdev_t g_msc_bdevs[MSC_MAX_DEVICES];

static int32_t msc_bdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    msc_bdev_t *b = (msc_bdev_t *)n->priv;
    if (!b || !b->sd || !b->sd->live)
        return -E_IO;

    /* The bus layer reads in whole device sectors; the unaligned head and
     * tail bounce through a bounce sector. */
    uint32_t ss    = b->sd->sector_size;
    uint64_t first = off / ss;
    uint64_t last  = (off + len - 1) / ss;
    uint8_t *out   = (uint8_t *)buf;

    static uint8_t sec[2048]; /* max sector size */
    for (uint64_t lba = first; lba <= last; lba++) {
        if (scsi_read_blocks(b->sd, lba, sec, 1) < 0)
            return -E_IO;
        uint64_t sec_start = lba * ss;
        uint64_t from      = off > sec_start ? off : sec_start;
        uint64_t to        = off + len < sec_start + ss ? off + len : sec_start + ss;
        if (to > from)
            memcpy(out + (from - off), sec + (from - sec_start), to - from);
    }
    return (int32_t)len;
}

static int32_t msc_bdev_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    (void)n;
    (void)off;
    (void)buf;
    (void)len;
    return -E_ROFS; /* deliberately read-only for now */
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

/* Probe a BOT-class interface the USB core has registered. */
int usb_storage_probe(usb_interface_t *interface)
{
    if (!interface || !interface->device)
        return -E_INVAL;
    if (interface->descriptor.interface_class != 0x08 ||    /* mass storage   */
        interface->descriptor.interface_subclass != 0x06 || /* transparent */
        interface->descriptor.interface_protocol != 0x50)   /* BOT         */
        return -E_OPNOTSUPP;

    usb_endpoint_t *ep_in  = usb_find_endpoint(interface, USB_ENDPOINT_XFER_BULK, true);
    usb_endpoint_t *ep_out = usb_find_endpoint(interface, USB_ENDPOINT_XFER_BULK, false);
    if (!ep_in || !ep_out)
        return -E_OPNOTSUPP;
    if (g_msc_count >= MSC_MAX_DEVICES)
        return -E_NOSPC;

    msc_dev_t *dev = &g_msc[g_msc_count];
    memset(dev, 0, sizeof(*dev));
    dev->ep_in  = ep_in;
    dev->ep_out = ep_out;

    /* The bus layer does the rest: INQUIRY, TEST UNIT READY (with retries
     * while the stick spins up), READ CAPACITY. */
    scsi_device_t sd;
    memset(&sd, 0, sizeof sd);
    sd.xport      = g_msc_transport;
    sd.xport.host = dev;
    sd.lun        = 0;
    if (scsi_device_add(&sd) < 0)
        return -E_IO;
    for (int i = 0; i < 10 && !sd.ready; i++) {
        timer_delay_ms(100);
        sd.ready = (scsi_test_unit_ready(&sd) == 0);
    }
    if (!sd.nblocks)
        return -E_IO;
    dev->nblocks = sd.nblocks;
    g_scsi       = scsi_device_by_index(scsi_count() - 1);

    dev->active   = 1;
    msc_bdev_t *b = &g_msc_bdevs[g_msc_count];
    b->dev        = dev;
    b->sd         = g_scsi;

    /* ATA owns sda; USB disks come next: sdb, sdc, ... */
    char name[8];
    name[0] = 's';
    name[1] = 'd';
    name[2] = (char)('b' + g_msc_count);
    name[3] = '\0';
    if (vfs_register_blkdev(name, &g_msc_bdev_ops, b, dev->nblocks * MSC_SECTOR) != 0) {
        dev->active = 0;
        return -E_IO;
    }

    dbg_puts("USBMS: /dev/");
    dbg_puts(name);
    dbg_puts(", ");
    dbg_puts_dec(dev->nblocks);
    dbg_puts(" sectors, ");
    dbg_puts(interface->device->path);
    dbg_puts("\r\n");
    g_msc_count++;
    return 0;
}

/* Disconnect the class driver from every storage interface of this device. */
void usb_storage_disconnect(usb_interface_t *interface)
{
    if (!interface || !interface->device)
        return;
    for (int i = 0; i < g_msc_count; i++) {
        msc_dev_t *dev = &g_msc[i];
        if (dev->active && dev->ep_in && dev->ep_in->interface &&
            dev->ep_in->interface->device == interface->device)
            dev->active = 0; /* the bdev reads start failing through sd->live */
    }
}
