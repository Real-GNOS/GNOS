#include "kernel/fs/dir.h"

#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/fs/fsapi.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"

struct FS_DIR root_dir;

void open_root_dir(struct DISK_PARTITION *part) {
    root_dir.inode = inode_open(part, 2);
    root_dir.dir_pos = 0;
}

#define MAX_OPEN_DIRS 16
static struct FS_DIR *dir_table[MAX_OPEN_DIRS];

struct FS_DIR *dir_open(struct DISK_PARTITION *part, uint32_t inode_no) {
    struct FS_DIR *pdir = (struct FS_DIR *)get_kernel_pages(1);
    if (pdir == NULL) {
        return NULL;
    }
    memset(pdir, 0, PAGE_SIZE);
    pdir->inode = inode_open(part, inode_no);
    pdir->dir_pos = 0;
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (dir_table[i] == NULL) {
            dir_table[i] = pdir;
            return (struct FS_DIR *)(uintptr_t)(i + 1);
        }
    }
    inode_close(pdir->inode);
    free_kernel_page((uint32_t)pdir);
    return NULL;
}

static struct FS_DIR *dir_handle_get(struct FS_DIR *handle) {
    uint32_t idx = (uint32_t)(uintptr_t)handle;
    if (idx == 0 || idx > MAX_OPEN_DIRS || dir_table[idx - 1] == NULL) {
        return NULL;
    }
    return dir_table[idx - 1];
}

void dir_close(struct FS_DIR *handle) {
    struct FS_DIR *d = dir_handle_get(handle);
    if (d == NULL) {
        return;
    }
    dir_table[(uint32_t)(uintptr_t)handle - 1] = NULL;
    inode_close(d->inode);
    free_kernel_page((uint32_t)d);
}

struct FS_DIRENT *dir_read(struct FS_DIR *handle) {
    struct FS_DIR *d = dir_handle_get(handle);
    if (d == NULL) {
        return NULL;
    }
    struct FS_DIRENT *dir_e = (struct FS_DIRENT *)d->dir_buf;
    if (fs_dir_next(d->inode, &d->dir_pos, dir_e)) {
        return NULL;
    }
    return dir_e;
}

void dir_rewind(struct FS_DIR *handle) {
    struct FS_DIR *d = dir_handle_get(handle);
    if (d != NULL) {
        d->dir_pos = 0;
    }
}
