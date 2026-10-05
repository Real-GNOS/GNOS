#ifndef FS_DIR_H
#define FS_DIR_H

#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include <stdint.h>

#define MAX_FILE_NAME_LEN 256

struct FS_DIR {
    struct FS_INODE *inode;
    uint32_t dir_pos;
    uint8_t dir_buf[512];
};

struct FS_DIRENT {
    char filename[MAX_FILE_NAME_LEN];
    uint32_t i_no;
    enum FS_FILE_TYPE f_type;
};

extern struct FS_DIR root_dir;

void open_root_dir(struct DISK_PARTITION *part);
struct FS_DIR *dir_open(struct DISK_PARTITION *part, uint32_t inode_no);
void dir_rewind(struct FS_DIR *dir);
void dir_close(struct FS_DIR *dir);
struct FS_DIRENT *dir_read(struct FS_DIR *dir);

#endif
