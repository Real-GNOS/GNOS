#include "kernel/fs/fsapi.h"

#include "ops/block_ops.h"
#include "drivers/block/block.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"

struct FS_OPS {
    int (*init)(void);
    struct DISK_PARTITION *(*partition)(void);
    int (*lookup)(const char *path, uint32_t *ino, int *is_dir);
    int (*lookup_ftype)(const char *path, uint32_t *ino, int *ftype, int follow);
    int (*abs_path)(const char *path, char *out, uint32_t cap);
    int (*read_link_target)(uint32_t ino, char *buf, uint32_t cap);
    int (*read_inode)(uint32_t ino, struct FS_INODE *out);
    int (*read_from_inode)(const struct FS_INODE *ino, uint32_t off, void *buf,
                           uint32_t count);
    int (*dir_next)(const struct FS_INODE *dino, uint32_t *pos,
                    struct FS_DIRENT *out);
    int (*new_inode)(uint32_t mode, struct FS_INODE *out);
    void (*free_inode)(uint32_t ino);
    int (*write_inode)(uint32_t ino, const struct FS_INODE *in);
    int (*write_to_inode)(struct FS_INODE *ino, uint32_t off, const void *buf,
                          uint32_t count);
    void (*truncate_inode)(struct FS_INODE *ino);
    int (*add_entry)(struct FS_INODE *dino, uint32_t ino, const char *name,
                     int is_dir);
    int (*add_entry_dt)(struct FS_INODE *dino, uint32_t ino, const char *name,
                        uint8_t dtype);
    int (*remove_entry)(struct FS_INODE *dino, const char *name);
    void (*statfs_info)(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                        uint32_t *files, uint32_t *ffree);
};

static const struct FS_OPS ext2_ops = {
    ext2_init,           ext2_partition,      ext2_lookup,
    ext2_lookup_ftype,   ext2_abs_path,       ext2_read_link_target,
    ext2_read_inode,     ext2_read_from_inode, ext2_dir_next,
    ext2_new_inode,      ext2_free_inode,     ext2_write_inode,
    ext2_write_to_inode, ext2_truncate_inode, ext2_add_entry,
    ext2_add_entry_dt,   ext2_remove_entry,   ext2_statfs_info,
};

static const struct FS_OPS ext4_ops = {
    ext4_init,           ext4_partition,      ext4_lookup,
    ext4_lookup_ftype,   ext4_abs_path,       ext4_read_link_target,
    ext4_read_inode,     ext4_read_from_inode, ext4_dir_next,
    ext4_new_inode,      ext4_free_inode,     ext4_write_inode,
    ext4_write_to_inode, ext4_truncate_inode, ext4_add_entry,
    ext4_add_entry_dt,   ext4_remove_entry,   ext4_statfs_info,
};

static const struct FS_OPS *g_ops = &ext2_ops;

#define FS_DRV_NONE 0
#define FS_DRV_EXT2 1
#define FS_DRV_EXT4 2

static int fs_probe(void) {
    struct LIST_ELEM *e = partition_list.head.next;
    while (e != &partition_list.tail) {
        struct DISK_PARTITION *p =
            list_entry(e, struct DISK_PARTITION, part_tag);
        uint8_t *buf = (uint8_t *)get_kernel_pages(1);
        if (buf == NULL) {
            return FS_DRV_NONE;
        }
        memset(buf, 0, PAGE_SIZE);
        BLOCK.read_sectors(p->my_disk, p->start_lba, buf, 4);
        uint8_t *sb = buf + 1024;
        int drv = FS_DRV_NONE;
        if (*(uint16_t *)(sb + 0x38) == EXT4_SUPER_MAGIC) {
            uint32_t incompat = *(uint32_t *)(sb + 0x60);
            uint32_t ro_compat = *(uint32_t *)(sb + 0x64);
            if ((incompat &
                 (EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT)) ||
                (ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {
                drv = FS_DRV_EXT4;
            } else {
                drv = FS_DRV_EXT2;
            }
        }
        free_kernel_page((uint32_t)buf);
        if (drv != FS_DRV_NONE) {
            return drv;
        }
        e = e->next;
    }
    return FS_DRV_NONE;
}

int fs_init(void) {
    g_ops = fs_probe() == FS_DRV_EXT4 ? &ext4_ops : &ext2_ops;
    return g_ops->init();
}

struct DISK_PARTITION *fs_partition(void) {
    return g_ops->partition();
}

int fs_lookup(const char *path, uint32_t *ino, int *is_dir) {
    return g_ops->lookup(path, ino, is_dir);
}

int fs_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow) {
    return g_ops->lookup_ftype(path, ino, ftype, follow);
}

int fs_abs_path(const char *path, char *out, uint32_t cap) {
    return g_ops->abs_path(path, out, cap);
}

int fs_read_link_target(uint32_t ino, char *buf, uint32_t cap) {
    return g_ops->read_link_target(ino, buf, cap);
}

int fs_read_inode(uint32_t ino, struct FS_INODE *out) {
    return g_ops->read_inode(ino, out);
}

int fs_read_from_inode(const struct FS_INODE *ino, uint32_t off, void *buf,
                       uint32_t count) {
    return g_ops->read_from_inode(ino, off, buf, count);
}

int fs_dir_next(const struct FS_INODE *dino, uint32_t *pos,
                struct FS_DIRENT *out) {
    return g_ops->dir_next(dino, pos, out);
}

int fs_new_inode(uint32_t mode, struct FS_INODE *out) {
    return g_ops->new_inode(mode, out);
}

void fs_free_inode(uint32_t ino) {
    g_ops->free_inode(ino);
}

int fs_write_inode(uint32_t ino, const struct FS_INODE *in) {
    return g_ops->write_inode(ino, in);
}

int fs_write_to_inode(struct FS_INODE *ino, uint32_t off, const void *buf,
                      uint32_t count) {
    return g_ops->write_to_inode(ino, off, buf, count);
}

void fs_truncate_inode(struct FS_INODE *ino) {
    g_ops->truncate_inode(ino);
}

int fs_add_entry(struct FS_INODE *dino, uint32_t ino, const char *name,
                 int is_dir) {
    return g_ops->add_entry(dino, ino, name, is_dir);
}

int fs_add_entry_dt(struct FS_INODE *dino, uint32_t ino, const char *name,
                    uint8_t dtype) {
    return g_ops->add_entry_dt(dino, ino, name, dtype);
}

int fs_remove_entry(struct FS_INODE *dino, const char *name) {
    return g_ops->remove_entry(dino, name);
}

void fs_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                    uint32_t *files, uint32_t *ffree) {
    g_ops->statfs_info(bsize, blocks, bfree, files, ffree);
}