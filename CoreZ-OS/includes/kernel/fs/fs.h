#ifndef FS_FS_H
#define FS_FS_H

#include "drivers/block/block.h"
#include <stdint.h>

#define MAX_FILES_PER_PART 4096
#define BITS_PER_SECTOR 4096
#define SECTOR_SIZE 512
#define BLOCK_SIZE SECTOR_SIZE
#define MAX_PATH_LEN 512
#define ROOT_DIR_INODE_NR 2

#define FS_MAGIC 0x19590318

enum FS_FILE_TYPE { FT_UNKNOWN, FT_REGULAR, FT_DIRECTORY, FT_CHARDEVICE,
                  FT_SYMLINK };

enum FS_OFLAGS { O_RDONLY, O_WRONLY, O_RDWR, O_CREAT = 4 };

enum FS_WHENCE { SEEK_SET = 1, SEEK_CUR, SEEK_END };

struct FS_STAT {
    uint32_t st_ino;
    uint32_t st_size;
    enum FS_FILE_TYPE st_filetype;
};

struct DISK_PARTITION;
struct FS_DIR;
struct FS_DIRENT;
struct FS_INODE;
extern struct DISK_PARTITION *cur_part;

void filesys_init(void);

int32_t block_bitmap_alloc(struct DISK_PARTITION *part);
void block_bitmap_free(struct DISK_PARTITION *part, uint32_t lba);
int32_t inode_bitmap_alloc(struct DISK_PARTITION *part);
void inode_bitmap_free(struct DISK_PARTITION *part, uint32_t inode_no);

char *path_parse(char *pathname, char *name_store);
int search_dir_entry(struct DISK_PARTITION *part, struct FS_DIR *pdir, const char *name,
                     struct FS_DIRENT *dir_e);
int search_file(const char *pathname);
int create_file(const char *pathname);
int create_file_mode(const char *pathname, uint32_t mode);
int open_file(const char *pathname, uint8_t flags);
int open_file_mode(const char *pathname, uint8_t flags, uint32_t mode);
uint32_t fs_dir_nlink(uint32_t ino);
int close_file(int fd);
uint32_t read_file(int fd, void *buf, uint32_t count);
uint32_t write_file(int fd, const void *buf, uint32_t count);
int32_t sys_lseek(int32_t fd, int32_t offset, uint8_t whence);
int sys_unlink(const char *pathname);
int32_t sys_mkdir(const char *pathname);
struct FS_DIR *sys_opendir(const char *name);
int32_t sys_closedir(struct FS_DIR *dir);
struct FS_DIRENT *sys_readdir(struct FS_DIR *dir);
void sys_rewinddir(struct FS_DIR *dir);
int32_t sys_rmdir(const char *pathname);
char *sys_getcwd(char *buf, uint32_t size);
int fs_cwd_abs_prefix(char *buf, uint32_t size);
int fs_inode_abs_path(uint32_t ino, char *buf, uint32_t size);
int32_t sys_chdir(const char *path);
int32_t sys_stat(const char *path, struct FS_STAT *buf);
int32_t sys_mknod(const char *path, uint32_t mode, uint32_t dev);
int32_t sys_chown(const char *path, uint32_t uid, uint32_t gid);
int fs_stat_full(const char *path, uint32_t *ino_no, uint32_t *size,
                 uint32_t *mode, uint32_t *uid, uint32_t *gid, int follow);
int32_t sys_symlink(const char *target, const char *linkpath);
int fs_check_perm(const struct FS_INODE *ino, uint32_t bits);
int fs_is_chardev(const struct FS_INODE *ino);
uint32_t fs_chardev_dev(const struct FS_INODE *ino);
int fs_rename_path(const char *oldpath, const char *newpath);
int fs_truncate_path(const char *path, uint32_t length);

#endif
