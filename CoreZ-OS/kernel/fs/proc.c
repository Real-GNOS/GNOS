#include "kernel/fs/proc.h"
#include "lib/str/str.h"
#include "libc/user/stdio.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/syscall/linux_abi.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/userprog/process.h"
#include "kernel/init/mb2.h"

enum {
    PROC_NONE,
    PROC_DIR,
    PROC_MEMINFO,
    PROC_STAT,
    PROC_STATUS,
    PROC_EXE,
    PROC_FD,
    PROC_MAPS,
    PROC_CMDLINE
};

int proc_match(const char *path) {
    if (path == NULL) {
        return 0;
    }
    if (strcmp(path, "/proc") == 0) {
        return 1;
    }
    return strncmp(path, "/proc/", 6) == 0;
}

static uint32_t proc_pid;
static uint32_t proc_pid_valid;

static uint32_t proc_task_of(uint32_t pid) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].slot_used && task_table[i].status != TASK_DIED &&
            task_table[i].pid == pid)
            return i;
    }
    return MAX_TASKS;
}

static int proc_node_of(const char *path) {
    if (strcmp(path, "/proc") == 0) {
        return PROC_DIR;
    }
    if (strcmp(path, "/proc/meminfo") == 0) {
        return PROC_MEMINFO;
    }
    if (strcmp(path, "/proc/cmdline") == 0) {
        return PROC_CMDLINE;
    }
    proc_pid_valid = 0;
    const char *p = path + 6;
    uint32_t pid = 0;
    if (strncmp(p, "self", 4) == 0 && current) {
        pid = current->pid;
        p += 4;
    } else {
        if (*p < '0' || *p > '9')
            return PROC_NONE;
        while (*p >= '0' && *p <= '9')
            pid = pid * 10u + (uint32_t)(*p++ - '0');
    }
    if (*p != '/')
        return PROC_NONE;
    p++;
    if (proc_task_of(pid) == MAX_TASKS)
        return PROC_NONE;
    proc_pid = pid;
    proc_pid_valid = 1;
    if (strcmp(p, "stat") == 0)
        return PROC_STAT;
    if (strcmp(p, "status") == 0)
        return PROC_STATUS;
    if (strcmp(p, "exe") == 0)
        return PROC_EXE;
    if (strcmp(p, "fd") == 0)
        return PROC_FD;
    if (strcmp(p, "maps") == 0)
        return PROC_MAPS;
    return PROC_NONE;
}

static uint32_t meminfo_build(char *dst, uint32_t cap) {
    uint32_t total_kb = kernel_pool.pool_size / 1024;
    uint32_t free_kb = kernel_pool_free_count() * (PAGE_SIZE / 1024);
    uint32_t used_kb = total_kb > free_kb ? total_kb - free_kb : 0;
    (void)cap;
    return sprintf(dst,
                   "MemTotal:     %d kB\n"
                   "MemFree:      %d kB\n"
                   "MemUsed:      %d kB\n",
                   total_kb, free_kb, used_kb);
}

static uint32_t procstat_build(char *dst, uint32_t cap, uint32_t slot) {
    struct TASK *t = &task_table[slot];
    char state = t->status == TASK_RUNNING ? 'R'
                 : (t->status == TASK_HANGING || t->status == TASK_DIED)
                     ? 'Z'
                     : (t->status == TASK_STOPPED ? 'T' : 'S');
    return sprintf(dst, "%d (%s) %c %d %d %d 0 0 0 0 0 0 0 0 %d 0 0 0\n",
                   t->pid, t->name, state,
                   t->parent_pid > 0 ? t->parent_pid : 1,
                   t->pgid ? t->pgid : t->pid, t->sid ? t->sid : t->pid,
                   t->elapsed_ticks);
}

static uint32_t procstatus_build(char *dst, uint32_t cap, uint32_t slot) {
    struct TASK *t = &task_table[slot];
    return sprintf(dst,
                   "Name:\t%s\nPid:\t%d\nPPid:\t%d\nUid:\t%d %d %d\n"
                   "Gid:\t%d %d %d\n",
                   t->name, t->pid, t->parent_pid > 0 ? t->parent_pid : 1,
                   t->uid, t->euid, t->suid, t->gid, t->egid, t->sgid);
}

static uint32_t procmaps_build(char *dst, uint32_t cap, uint32_t slot) {
    struct TASK *t = &task_table[slot];
    uint32_t n = 0;
    (void)cap;
    if (t->user_brk > t->brk_base) {
        n += sprintf(dst + n, "%x-%x rw-p 00000000 00:00 0 [heap]\n",
                     t->brk_base, t->user_brk);
    }
    n += sprintf(dst + n, "%x-%x rw-p 00000000 00:00 0 [stack]\n",
                 t->stack_bottom, USER_STACK_TOP);
    return n;
}

static uint32_t proc_size(int node) {
    char buf[256];
    if (node == PROC_MEMINFO) {
        return meminfo_build(buf, sizeof(buf));
    }
    if (node == PROC_CMDLINE) {
        const struct MB2_INFO *bi = mb2_get();
        const char *cl = bi && bi->cmdline ? bi->cmdline : "";
        uint32_t n = (uint32_t)strlen(cl);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        memcpy(buf, cl, n);
        buf[n] = '\n';
        return n + 1;
    }
    if (node == PROC_STAT && proc_pid_valid) {
        return procstat_build(buf, sizeof(buf), proc_task_of(proc_pid));
    }
    if (node == PROC_STATUS && proc_pid_valid) {
        return procstatus_build(buf, sizeof(buf), proc_task_of(proc_pid));
    }
    if (node == PROC_MAPS && proc_pid_valid) {
        return procmaps_build(buf, sizeof(buf), proc_task_of(proc_pid));
    }
    if (node == PROC_EXE && proc_pid_valid) {
        uint32_t slot = proc_task_of(proc_pid);
        if (slot != MAX_TASKS) {
            return (uint32_t)strlen(task_table[slot].exe_path);
        }
    }
    return 0;
}

int proc_open(const char *path, uint8_t flags) {
    int node = proc_node_of(path);
    if (node == PROC_NONE) {
        return -1;
    }
    int gfd = file_table_alloc_slot();
    if (gfd == -1) {
        return -1;
    }
    struct FILE *file = file_get((uint32_t)gfd);
    file->fd_pos = 0;
    file->fd_flag = (node == PROC_DIR || node == PROC_FD) ? PROC_DIRF_FLAG
                                                          : flags;
    file->fd_inode = NULL;
    file->proc_id = (uint32_t)node;
    file->proc_aux = (node == PROC_STAT || node == PROC_STATUS ||
                      node == PROC_FD)
                         ? proc_pid
                         : 0;
    file->ref_cnt = 1;
    int fd = fd_install(gfd);
    if (fd == -1) {
        file_table_free_slot(gfd);
        return -1;
    }
    return fd;
}

uint32_t proc_read(struct FILE *file, void *buf, uint32_t count) {
    char info[256];
    uint32_t len;
    if (file->proc_id == PROC_MEMINFO) {
        len = meminfo_build(info, sizeof(info));
    } else if (file->proc_id == PROC_CMDLINE) {
        const struct MB2_INFO *bi = mb2_get();
        const char *cl = bi && bi->cmdline ? bi->cmdline : "";
        uint32_t n = (uint32_t)strlen(cl);
        if (n >= sizeof(info))
            n = sizeof(info) - 1;
        memcpy(info, cl, n);
        info[n] = '\n';
        len = n + 1;
    } else if (file->proc_id == PROC_MAPS) {
        uint32_t slot = proc_task_of(file->proc_aux);
        if (slot == MAX_TASKS)
            return 0;
        len = procmaps_build(info, sizeof(info), slot);
    } else if (file->proc_id == PROC_STAT || file->proc_id == PROC_STATUS) {
        uint32_t slot = proc_task_of(file->proc_aux);
        if (slot == MAX_TASKS)
            return 0;
        len = file->proc_id == PROC_STAT
                  ? procstat_build(info, sizeof(info), slot)
                  : procstatus_build(info, sizeof(info), slot);
    } else {
        return 0;
    }
    if (file->fd_pos >= len) {
        return 0;
    }
    uint32_t remain = len - file->fd_pos;
    uint32_t n = (count < remain) ? count : remain;
    memcpy(buf, info + file->fd_pos, n);
    file->fd_pos += n;
    return n;
}

int proc_stat(const char *path, struct FS_STAT *buf) {
    int node = proc_node_of(path);
    if (node == PROC_NONE) {
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->st_ino = 1;
    if (node == PROC_DIR || node == PROC_FD) {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_size = 0;
    } else if (node == PROC_EXE) {
        buf->st_filetype = FT_SYMLINK;
        buf->st_size = proc_size(node);
    } else {
        buf->st_filetype = FT_REGULAR;
        buf->st_size = proc_size(node);
    }
    return 0;
}

int proc_fstat(struct FILE *file, struct FS_STAT *buf) {
    if (file->proc_id == PROC_NONE) {
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->st_ino = 1;
    if (file->proc_id == PROC_DIR || file->proc_id == PROC_FD) {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_size = 0;
    } else if (file->proc_id == PROC_EXE) {
        buf->st_filetype = FT_SYMLINK;
        buf->st_size = proc_size((int)file->proc_id);
    } else {
        buf->st_filetype = FT_REGULAR;
        buf->st_size = proc_size((int)file->proc_id);
    }
    return 0;
}

int proc_access(const char *path) {
    return proc_match(path) ? 0 : -1;
}

int proc_lseek(struct FILE *file, int32_t offset, uint8_t whence) {
    int32_t size = (int32_t)proc_size(file->proc_id);
    int32_t new_pos = 0;
    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        new_pos = (int32_t)file->fd_pos + offset;
    } else if (whence == SEEK_END) {
        new_pos = size + offset;
    } else {
        return -1;
    }
    if (new_pos < 0) {
        return -1;
    }
    file->fd_pos = (uint32_t)new_pos;
    return (int32_t)file->fd_pos;
}

int proc_readlink(const char *path, char *buf, uint32_t bufsiz) {
    int node = proc_node_of(path);
    if (node != PROC_EXE) {
        current->errno = 2;
        return -1;
    }
    uint32_t slot = proc_pid_valid ? proc_task_of(proc_pid) : MAX_TASKS;
    if (slot == MAX_TASKS || task_table[slot].exe_path[0] == 0) {
        current->errno = 2;
        return -1;
    }
    uint32_t len = (uint32_t)strlen(task_table[slot].exe_path);
    uint32_t n = len < bufsiz ? len : bufsiz;
    memcpy(buf, task_table[slot].exe_path, n);
    return (int32_t)n;
}

int32_t proc_getdents64(struct FILE *file, void *dirp, uint32_t count) {
    if (file->proc_id != PROC_FD) {
        return 0;
    }
    uint32_t slot = proc_task_of(file->proc_aux);
    if (slot == MAX_TASKS) {
        return 0;
    }
    uint32_t written = 0;
    for (uint32_t fd = file->fd_pos; fd < MAX_FILES_OPEN_PER_PROC; fd++) {
        if (task_table[slot].fd_table[fd] == (uint32_t)-1) {
            continue;
        }
        char name[12];
        uint32_t nl = sprintf(name, "%d", fd);
        uint16_t reclen = (uint16_t)((19u + nl + 1u + 7u) & ~7u);
        if (written + reclen > count) {
            break;
        }
        struct LINUX_DIRENT64 *d =
            (struct LINUX_DIRENT64 *)((uint8_t *)dirp + written);
        d->d_ino = 1;
        d->d_off = (int64_t)(fd + 1);
        d->d_reclen = reclen;
        d->d_type = LINUX_DT_LNK;
        memcpy(d->d_name, name, nl + 1);
        written += reclen;
        file->fd_pos = fd + 1;
    }
    return (int32_t)written;
}
