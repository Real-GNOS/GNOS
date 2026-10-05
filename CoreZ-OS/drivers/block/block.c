#include "drivers/block/block.h"

#include <stdint.h>

#include "drivers/block/ide.h"
#include "drivers/block/nvme.h"
#include "drivers/char/console/io.h"
#include "drivers/driver_ops.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"
#include "libc/user/stdio.h"
#include "ops/block_ops.h"

static uint8_t prim_nr;
static uint8_t logic_nr;
static uint32_t ext_lba_base;
struct LIST partition_list;

static int block_read(void *dev, uint32_t lba, void *buf, uint32_t count) {
    struct DISK *hd = (struct DISK *)dev;

    if (hd->kind == DISK_KIND_NVME) {
        return nvme_read_sectors(hd, lba, buf, count);
    }
    ide_read(hd, lba, buf, count);
    return 0;
}

static int block_write(void *dev, uint32_t lba, const void *buf,
                       uint32_t count) {
    struct DISK *hd = (struct DISK *)dev;

    if (hd->kind == DISK_KIND_NVME) {
        return nvme_write_sectors(hd, lba, buf, count);
    }
    ide_write(hd, lba, buf, count);
    return 0;
}

const struct BLOCK_OPS BLOCK = {
    .read_sectors = block_read,
    .write_sectors = block_write,
};

static void register_partition(struct DISK *hd, uint32_t start_lba,
                               uint32_t sec_cnt, int logical) {
    struct DISK_PARTITION *p;

    if (logical) {
        p = &hd->logic_parts[logic_nr];
        sprintf(p->name, "%s%d", hd->name, logic_nr + 5);
        logic_nr++;
    } else {
        p = &hd->prim_parts[prim_nr];
        sprintf(p->name, "%s%d", hd->name, prim_nr + 1);
        prim_nr++;
    }
    p->start_lba = ext_lba_base + start_lba;
    p->sec_cnt = sec_cnt;
    p->my_disk = hd;
    list_append(&partition_list, &p->part_tag);
}

static void scan_partitions(struct DISK *hd, uint32_t ext_lba) {
    struct DISK_BOOT_SECTOR *bs = (struct DISK_BOOT_SECTOR *)get_kernel_pages(1);

    if (bs == NULL) {
        return;
    }
    BLOCK.read_sectors(hd, ext_lba, bs, 1);
    struct DISK_PART_ENTRY *p = bs->partition_table;
    for (uint32_t i = 0; i < 4; i++, p++) {
        if (p->fs_type == 0x5) {
            if (ext_lba == 0) {
                ext_lba_base = p->start_lba;
                scan_partitions(hd, p->start_lba);
            } else {
                scan_partitions(hd, p->start_lba + ext_lba_base);
            }
        } else if (p->fs_type != 0) {
            if (ext_lba == 0) {
                register_partition(hd, p->start_lba, p->sec_cnt, 0);
            } else {
                register_partition(hd, p->start_lba, p->sec_cnt, 1);
                if (logic_nr >= 8) {
                    free_kernel_page((uint32_t)bs);
                    return;
                }
            }
        }
    }
    free_kernel_page((uint32_t)bs);
}

void block_scan_partitions(struct DISK *hd) {
    prim_nr = 0;
    logic_nr = 0;
    ext_lba_base = 0;
    scan_partitions(hd, 0);
}

void block_print_partitions(void) {
    struct LIST_ELEM *e = partition_list.head.next;
    while (e != &partition_list.tail) {
        struct DISK_PARTITION *part =
            list_entry(e, struct DISK_PARTITION, part_tag);
        kprintf("    %s start_lba:0x%x, sec_cnt:0x%x\n", part->name,
                part->start_lba, part->sec_cnt);
        e = e->next;
    }
}

static int block_init(void) {
    list_init(&partition_list);
    return 0;
}

DRIVER_REGISTER("block", 9, block_init);
