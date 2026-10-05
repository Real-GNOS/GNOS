#ifndef FS_INODE_H
#define FS_INODE_H

#include "lib/list/list.h"
#include "lib/rbtree/rbtree.h"
#include <stdint.h>

struct FS_INODE {
    uint32_t i_no;
    uint32_t i_size;
    uint32_t i_mode;
    uint32_t i_uid;
    uint32_t i_gid;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_links_count;
    uint32_t i_flags;
    uint32_t i_blocks;
    uint32_t i_dtime;
    uint32_t i_generation;
    uint32_t i_open_cnt;
    uint8_t write_deny;
    uint32_t i_block[15];
    struct RB_NODE inode_rb_node;
};

struct DISK_PARTITION;

struct FS_INODE *inode_open(struct DISK_PARTITION *part, uint32_t inode_no);
void inode_close(struct FS_INODE *inode);

#endif
