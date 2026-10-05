#ifndef FS_FSAPI_H
#define FS_FSAPI_H

#include "kernel/fs/dir.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/ext4.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include <stdint.h>

#define FS_DT_DIR 2u
#define FS_DT_CHR 3u
#define FS_DT_LNK 7u

int fs_init(void);
struct DISK_PARTITION *fs_partition(void);
int fs_lookup(const char *path, uint32_t *ino, int *is_dir);
int fs_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow);
int fs_abs_path(const char *path, char *out, uint32_t cap);
int fs_read_link_target(uint32_t ino, char *buf, uint32_t cap);
int fs_read_inode(uint32_t ino, struct FS_INODE *out);
int fs_read_from_inode(const struct FS_INODE *ino, uint32_t off, void *buf,
                       uint32_t count);
int fs_dir_next(const struct FS_INODE *dino, uint32_t *pos,
                struct FS_DIRENT *out);

int fs_new_inode(uint32_t mode, struct FS_INODE *out);
void fs_free_inode(uint32_t ino);
int fs_write_inode(uint32_t ino, const struct FS_INODE *in);
int fs_write_to_inode(struct FS_INODE *ino, uint32_t off, const void *buf,
                      uint32_t count);
void fs_truncate_inode(struct FS_INODE *ino);
int fs_add_entry(struct FS_INODE *dino, uint32_t ino, const char *name,
                 int is_dir);
int fs_add_entry_dt(struct FS_INODE *dino, uint32_t ino, const char *name,
                    uint8_t dtype);
int fs_remove_entry(struct FS_INODE *dino, const char *name);

#ifndef __ASSEMBLER__
void fs_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                    uint32_t *files, uint32_t *ffree);
#endif

#endif