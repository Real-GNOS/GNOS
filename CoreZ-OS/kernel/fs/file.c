#include "kernel/fs/file.h"
#include "kernel/asm_func.h"
#include "kernel/sched/sync.h"
#include "kernel/sched/thread.h"
#include "kernel/fs/fsapi.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include "drivers/char/tty.h"
#include "drivers/char/pty.h"
#include "lib/rand/rand.h"
#include "lib/str/str.h"
struct FILE file_table[MAX_FILE_OPEN];

static volatile uint32_t file_slot_used[MAX_FILE_OPEN / 32];

void file_table_init(void) {
    for (uint32_t w = 0; w < MAX_FILE_OPEN / 32; w++)
        file_slot_used[w] = 0;
    file_slot_used[0] = 0x7u;
    for (uint32_t i = 0; i < 3; i++) {
        file_table[i].fd_inode = FILE_SLOT_RESERVED;
        file_table[i].ref_cnt = 1;
    }
}

int file_table_alloc_slot(void) {
    for (;;) {
        int w = -1;
        uint32_t b = 0;
        for (uint32_t k = 0; k < MAX_FILE_OPEN / 32; k++) {
            uint32_t cur = cpu_atomic_load32(&file_slot_used[k]);
            if (~cur != 0) {
                w = (int)k;
                b = cur;
                break;
            }
        }
        if (w < 0) {
            return -1;
        }
        uint32_t free_bits = ~b;
        uint32_t i = (uint32_t)__builtin_ctz(free_bits);
        if (cpu_cmpxchg32(&file_slot_used[w], b, b | (1u << i)) != b) {
            continue;
        }
        int idx = w * 32 + (int)i;
        file_table[idx].fd_pos = 0;
        file_table[idx].fd_flag = 0;
        file_table[idx].fd_nonblock = 0;
        file_table[idx].fd_inode = FILE_SLOT_RESERVED;
        file_table[idx].proc_id = 0;
        file_table[idx].proc_aux = 0;
        file_table[idx].ref_cnt = 0;
        file_table[idx].dev_priv = 0;
        return idx;
    }
}

void file_table_free_slot(int idx) {
    if (idx < 0 || idx >= (int)MAX_FILE_OPEN) {
        return;
    }
    file_table[idx].fd_inode = NULL;
    file_table[idx].fd_pos = 0;
    file_table[idx].fd_flag = 0;
    file_table[idx].proc_id = 0;
    file_table[idx].proc_aux = 0;
    file_table[idx].ref_cnt = 0;
    uint32_t w = (uint32_t)idx / 32;
    uint32_t mask = 1u << (idx % 32);
    for (;;) {
        uint32_t b = cpu_atomic_load32(&file_slot_used[w]);
        if (!(b & mask)) {
            break;
        }
        if (cpu_cmpxchg32(&file_slot_used[w], b, b & ~mask) == b) {
            break;
        }
    }
}

struct FILE *file_get(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return NULL;
    }
    return &file_table[gfd];
}

void file_table_ref(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return;
    }
    cpu_xadd32(&file_table[gfd].ref_cnt, 1);
}

uint32_t file_table_unref(uint32_t gfd) {
    if (gfd >= MAX_FILE_OPEN) {
        return 0;
    }
    uint32_t old = cpu_xadd32(&file_table[gfd].ref_cnt, (uint32_t)-1);
    if (old == 0) {
        file_table[gfd].ref_cnt = 0;
        return 0;
    }
    return old - 1;
}

int fd_install(int32_t global_fd_idx) {
    return fd_install_from(global_fd_idx, 3);
}

struct TASK *fd_owner_task(void) {
    int32_t owner_pid = current->fd_owner_pid;
    if (owner_pid <= 0 || owner_pid == (int32_t)current->pid)
        return current;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct TASK *t = &task_table[i];
        if (t->slot_used && t->status != TASK_DIED &&
            (int32_t)t->pid == owner_pid)
            return t;
    }
    return current;
}

int fd_install_from(int32_t global_fd_idx, uint32_t min_local) {
    struct TASK *owner = fd_owner_task();
    uint32_t local_fd = min_local < 3 ? 3 : min_local;
    while (local_fd < MAX_FILES_OPEN_PER_PROC) {
        if (owner->fd_table[local_fd] == (uint32_t)-1) {
            owner->fd_table[local_fd] = (uint32_t)global_fd_idx;
            owner->pipe_wr_mask &= ~(1ull << local_fd);
            owner->fd_cloexec &= ~(1ull << local_fd);
            return (int)local_fd;
        }
        local_fd++;
    }
    return -1;
}

int fd_release(uint32_t local_fd) {
    if (local_fd >= MAX_FILES_OPEN_PER_PROC) {
        return -1;
    }
    struct TASK *owner = fd_owner_task();
    owner->fd_table[local_fd] = (uint32_t)-1;
    owner->pipe_wr_mask &= ~(1ull << local_fd);
    owner->fd_cloexec &= ~(1ull << local_fd);
    return 0;
}

uint32_t fd_local2global(uint32_t local_fd) {
    if (local_fd >= MAX_FILES_OPEN_PER_PROC) {
        return (uint32_t)-1;
    }
    return fd_owner_task()->fd_table[local_fd];
}

static uint32_t chardev_read(struct FILE *file, void *buf, uint32_t count) {
    if (file->dev_priv) {
        int32_t r = pty_chardev_read(file, buf, count);
        return r < 0 ? 0 : (uint32_t)r;
    }
    uint32_t dev = file->fd_inode->i_block[0];
    if (dev >> 8 == 5u)
        return (uint32_t)TTY.read((char *)buf, count);
    if (dev == 0x0105u) {
        memset(buf, 0, count);
        return count;
    }
    if (dev == 0x0108u || dev == 0x0109u || dev == 0x0101u) {
        rand_bytes(buf, count);
        return count;
    }
    return 0;
}

static uint32_t chardev_write(struct FILE *file, const void *buf,
                              uint32_t count) {
    if (file->dev_priv) {
        int32_t r = pty_chardev_write(file, buf, count);
        return r < 0 ? 0 : (uint32_t)r;
    }
    if (file->fd_inode->i_block[0] >> 8 == 5u)
        return (uint32_t)TTY.write((const char *)buf, count);
    return count;
}

uint32_t file_read(struct FILE *file, void *buf, uint32_t count) {
    if (fs_is_chardev(file->fd_inode))
        return chardev_read(file, buf, count);
    int r = fs_read_from_inode(file->fd_inode, file->fd_pos, buf, count);
    file->fd_pos += (uint32_t)r;
    return (uint32_t)r;
}

uint32_t file_write(struct FILE *file, const void *buf, uint32_t count) {
    if (fs_is_chardev(file->fd_inode))
        return chardev_write(file, buf, count);
    int r = fs_write_to_inode(file->fd_inode, file->fd_pos, buf, count);
    file->fd_pos += (uint32_t)r;
    return (uint32_t)r;
}
