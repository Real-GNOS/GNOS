#ifndef DRIVERS_BLOCK_BLOCK_H
#define DRIVERS_BLOCK_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs/super_block.h"
#include "kernel/mm/bitmap/bitmap.h"
#include "lib/list/list.h"
#include "lib/rbtree/rbtree.h"

struct IDE_CHANNEL;

struct DISK_PARTITION {
    uint32_t start_lba;
    uint32_t sec_cnt;
    struct DISK *my_disk;
    struct LIST_ELEM part_tag;
    char name[8];
    struct FS_SUPER_BLOCK *sb;
    struct MM_BITMAP block_bitmap;
    struct MM_BITMAP inode_bitmap;
    struct RB_ROOT open_inodes_rb;
};

#define DISK_KIND_IDE 0u
#define DISK_KIND_NVME 1u

struct DISK {
    char name[8];
    struct IDE_CHANNEL *my_channel;
    uint8_t dev_no;
    uint8_t kind;
    void *drv_priv;
    uint32_t max_lba;
    struct DISK_PARTITION prim_parts[4];
    struct DISK_PARTITION logic_parts[8];
};

struct DISK_PART_ENTRY {
    uint8_t bootable;
    uint8_t start_head;
    uint8_t start_sec;
    uint8_t start_chs;
    uint8_t fs_type;
    uint8_t end_head;
    uint8_t end_sec;
    uint8_t end_chs;
    uint32_t start_lba;
    uint32_t sec_cnt;
} __attribute__((packed));

struct DISK_BOOT_SECTOR {
    uint8_t other[446];
    struct DISK_PART_ENTRY partition_table[4];
    uint16_t signature;
} __attribute__((packed));

_Static_assert(sizeof(struct DISK_PART_ENTRY) == 16,
               "partition_table_entry must be exactly 16 bytes (MBR spec)");
_Static_assert(sizeof(struct DISK_BOOT_SECTOR) == 512,
               "boot_sector must be exactly 512 bytes (one sector)");
_Static_assert(offsetof(struct DISK_BOOT_SECTOR, partition_table) == 446,
               "partition_table must start at offset 446 in boot_sector");

void block_scan_partitions(struct DISK *hd);
void block_print_partitions(void);

extern struct LIST partition_list;

#endif
