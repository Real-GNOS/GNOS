#ifndef FS_EXT4_H
#define FS_EXT4_H

#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include <stdint.h>

struct DISK_PARTITION;

#define EXT4_SUPER_MAGIC 0xEF53u
#define EXT4_INODE_SIZE 256u
#define EXT4_DIRENT_NAME_MAX 255u

#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040u
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080u
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200u
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000u

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE 0x0002u
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE 0x0008u
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK 0x0020u
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040u
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400u

#define EXT4_FEATURE_SUPPORTED_INCOMPAT                                  \
    (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS |    \
     EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG |       \
     EXT4_FEATURE_INCOMPAT_CSUM_SEED)

#define EXT4_FEATURE_SUPPORTED_RO                                        \
    (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |                               \
     EXT4_FEATURE_RO_COMPAT_LARGE_FILE |                                 \
     EXT4_FEATURE_RO_COMPAT_HUGE_FILE |                                  \
     EXT4_FEATURE_RO_COMPAT_DIR_NLINK |                                  \
     EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |                                \
     EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)

#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_EXTENT_MAGIC 0xF30Au
#define EXT4_EXTENT_ROOT_MAX 4u

#define EXT4_BG_INODE_UNINIT 0x0001u
#define EXT4_BG_BLOCK_UNINIT 0x0002u
#define EXT4_BG_INODE_ZEROED 0x0004u

#define EXT4_DT_UNKNOWN 0u
#define EXT4_DT_REG 1u
#define EXT4_DT_DIR 2u
#define EXT4_DT_CHR 3u
#define EXT4_DT_BLK 4u
#define EXT4_DT_FIFO 5u
#define EXT4_DT_SOCK 6u
#define EXT4_DT_LNK 7u

#define EXT4_INODE_EXTRA_ISIZE 32u

#define EXT4_JBD2_MAGIC 0xC03B3998u
#define EXT4_JT_DESCRIPTOR 1u
#define EXT4_JT_COMMIT 2u
#define EXT4_JF_ESCAPE 0x1u
#define EXT4_JF_SAME_UUID 0x2u
#define EXT4_JF_DELETED 0x4u
#define EXT4_JF_LAST_TAG 0x8u

struct EXT4_DIRENT {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[0];
} __attribute__((packed));

struct EXT4_DIR_TAIL {
    uint32_t det_reserved_zero1;
    uint16_t det_rec_len;
    uint8_t det_reserved_zero2;
    uint8_t det_reserved_ft;
    uint32_t det_checksum;
} __attribute__((packed));

struct EXT4_EXTENT_HEADER {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed));

struct EXT4_EXTENT {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed));

struct EXT4_EXTENT_IDX {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed));

int ext4_init(void);
struct DISK_PARTITION *ext4_partition(void);
int ext4_lookup(const char *path, uint32_t *ino, int *is_dir);
int ext4_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow);
int ext4_abs_path(const char *path, char *out, uint32_t cap);
int ext4_read_link_target(uint32_t ino, char *buf, uint32_t cap);
int ext4_read_inode(uint32_t ino, struct FS_INODE *out);
int ext4_read_from_inode(const struct FS_INODE *ino, uint32_t off, void *buf,
                         uint32_t count);
int ext4_dir_next(const struct FS_INODE *dino, uint32_t *pos,
                  struct FS_DIRENT *out);

int ext4_new_inode(uint32_t mode, struct FS_INODE *out);
void ext4_free_inode(uint32_t ino);
int ext4_write_inode(uint32_t ino, const struct FS_INODE *in);
int ext4_write_to_inode(struct FS_INODE *ino, uint32_t off, const void *buf,
                        uint32_t count);
void ext4_truncate_inode(struct FS_INODE *ino);
int ext4_add_entry(struct FS_INODE *dino, uint32_t ino, const char *name,
                   int is_dir);
int ext4_add_entry_dt(struct FS_INODE *dino, uint32_t ino, const char *name,
                      uint8_t dtype);
int ext4_remove_entry(struct FS_INODE *dino, const char *name);

#ifndef __ASSEMBLER__
void ext4_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                      uint32_t *files, uint32_t *ffree);
#endif

#endif