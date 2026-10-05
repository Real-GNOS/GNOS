#ifndef FS_EXT2_H
#define FS_EXT2_H

#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include <stdint.h>

struct DISK_PARTITION;
#define EXT2_SUPER_MAGIC 0xEF53u
#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFDIR 0x4000u
#define EXT2_DT_DIR 2u
#define EXT2_DT_LNK 7u
#define EXT2_INODE_SIZE 128u

struct EXT2_SURPER {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
} __attribute__((packed));

struct EXT2_DIRENT {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[0];
} __attribute__((packed));

int ext2_init(void);
struct DISK_PARTITION *ext2_partition(void);
int ext2_lookup(const char *path, uint32_t *ino, int *is_dir);
int ext2_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow);
int ext2_abs_path(const char *path, char *out, uint32_t cap);
int ext2_read_link_target(uint32_t ino, char *buf, uint32_t cap);
int ext2_read_inode(uint32_t ino, struct FS_INODE *out);
int ext2_read_from_inode(const struct FS_INODE *ino, uint32_t off, void *buf,
                         uint32_t count);
int ext2_dir_next(const struct FS_INODE *dino, uint32_t *pos,
                  struct FS_DIRENT *out);

int ext2_new_inode(uint32_t mode, struct FS_INODE *out);
void ext2_free_inode(uint32_t ino);
int ext2_write_inode(uint32_t ino, const struct FS_INODE *in);
int ext2_write_to_inode(struct FS_INODE *ino, uint32_t off, const void *buf,
                        uint32_t count);
void ext2_truncate_inode(struct FS_INODE *ino);
int ext2_add_entry(struct FS_INODE *dino, uint32_t ino, const char *name,
                   int is_dir);
int ext2_add_entry_dt(struct FS_INODE *dino, uint32_t ino, const char *name,
                       uint8_t dtype);
int ext2_remove_entry(struct FS_INODE *dino, const char *name);

#ifndef __ASSEMBLER__
void ext2_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                      uint32_t *files, uint32_t *ffree);
#endif

#endif
