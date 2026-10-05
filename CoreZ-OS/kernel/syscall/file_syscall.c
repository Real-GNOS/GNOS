#include "kernel/syscall/file_syscall.h"
#include "drivers/block/ide.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/fsapi.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/inode.h"
#include "kernel/fs/proc.h"
#include "kernel/shell/pipe.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "drivers/net/socket.h"
#include "kernel/sched/thread.h"
struct LINUX_DIRENT {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    char d_name[1];
};
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
static struct FILE *fd_lookup(int32_t fd) {
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC) {
        return NULL;
    }
    uint32_t global_fd = fd_owner_task()->fd_table[fd];
    if (global_fd == (uint32_t)-1 || global_fd >= MAX_FILE_OPEN) {
        return NULL;
    }
    return file_get(global_fd);
}

int32_t sys_fstat(int32_t fd, void *buf) {
    if (buf == NULL) {
        return -1;
    }
    struct FILE *pf = fd_lookup(fd);
    if (pf == NULL || (pf->fd_inode == NULL && pf->proc_id == 0)) {
        return -1;
    }
    struct FS_STAT *st = (struct FS_STAT *)buf;
    memset(st, 0, sizeof(*st));
    if (pf->proc_id != 0) {
        return proc_fstat(pf, st);
    }
    st->st_ino = pf->fd_inode->i_no;
    st->st_size = pf->fd_inode->i_size;
    st->st_filetype = FT_REGULAR;
    return 0;
}

int32_t sys_dup(int32_t oldfd) {
    struct FILE *pf = fd_lookup(oldfd);
    if (pf == NULL) {
        return -1;
    }
    uint32_t global_fd = (uint32_t)(pf - file_table);

    file_table_ref(global_fd);
    int newfd = fd_install((int32_t)global_fd);
    if (newfd == -1) {
        file_table_unref(global_fd);
        return -1;
    }
    return newfd;
}

int32_t sys_dup_from(int32_t oldfd, uint32_t min_local) {
    struct FILE *pf = fd_lookup(oldfd);
    if (pf == NULL) {
        current->errno = 9;
        return -1;
    }
    uint32_t global_fd = (uint32_t)(pf - file_table);
    file_table_ref(global_fd);
    int newfd = fd_install_from((int32_t)global_fd, min_local);
    if (newfd == -1) {
        file_table_unref(global_fd);
        current->errno = 24;
        return -1;
    }
    return newfd;
}

int32_t sys_dup2(int32_t oldfd, int32_t newfd) {
    if (newfd < 0 || newfd >= (int32_t)MAX_FILES_OPEN_PER_PROC) {
        return -1;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    struct FILE *pf = fd_lookup(oldfd);
    if (pf == NULL) {
        return -1;
    }
    uint32_t global_fd = (uint32_t)(pf - file_table);

    file_table_ref(global_fd);
    struct TASK *dup_owner = fd_owner_task();
    if (dup_owner->fd_table[newfd] != (uint32_t)-1) {
        close_file(newfd);
    }
    dup_owner->fd_table[newfd] = global_fd;
    return newfd;
}

int32_t sys_fcntl(int32_t fd, int32_t cmd, uint32_t arg) {
    if (net_is_socket(fd))
        return net_fcntl(fd, cmd, arg);
    struct FILE *pf = fd_lookup(fd);
    if (pf == NULL) {
        return -1;
    }
    struct TASK *fd_task = fd_owner_task();
    switch (cmd) {
    case F_DUPFD:
        return sys_dup_from(fd, (uint32_t)arg);
    case F_GETFD:
        return (int32_t)((fd_task->fd_cloexec >> fd) & 1);
    case F_SETFD:
        if (arg & 1)
            fd_task->fd_cloexec |= (1ull << fd);
        else
            fd_task->fd_cloexec &= ~(1ull << fd);
        return 0;
    case F_GETFL:
        if (is_pipe((uint32_t)fd))
            return pf->fd_nonblock ? O_NONBLOCK : 0;
        return (int32_t)(pf->fd_flag | (pf->fd_nonblock ? O_NONBLOCK : 0));
    case F_SETFL:
        pf->fd_nonblock = (arg & O_NONBLOCK) ? 1 : 0;
        if (!is_pipe((uint32_t)fd))
            pf->fd_flag = (pf->fd_flag & 3u) | (arg & ~3u);
        return 0;
    case F_DUPFD_CLOEXEC: {
        int32_t nfd = sys_dup_from(fd, arg);
        if (nfd < 0)
            return nfd;
        fd_task->fd_cloexec |= (1ull << nfd);
        return nfd;
    }
    default:
        return -1;
    }
}

int32_t sys_getdents(int32_t fd, void *dirp, uint32_t count) {
    if (dirp == NULL) {
        return -1;
    }
    struct FILE *pf = fd_lookup(fd);
    if (pf == NULL || pf->fd_inode == NULL) {
        return -1;
    }
    uint32_t pos = 0;
    struct FS_DIRENT de;
    uint32_t written = 0;
    while (fs_dir_next(pf->fd_inode, &pos, &de) == 0) {
        uint32_t name_len = strlen(de.filename);
        uint16_t reclen = (uint16_t)(10u + name_len + 1u);
        if (written + reclen > count) {
            break;
        }
        struct LINUX_DIRENT *ld =
            (struct LINUX_DIRENT *)((uint8_t *)dirp + written);
        ld->d_ino = de.i_no;
        ld->d_off = written + reclen;
        ld->d_reclen = reclen;
        memcpy(ld->d_name, de.filename, name_len + 1);
        written += reclen;
    }
    return (int32_t)written;
}

int32_t sys_readlink(const char *path, char *buf, uint32_t bufsiz) {
    if (path == NULL || buf == NULL || bufsiz == 0) {
        return -1;
    }
    if (proc_match(path)) {
        return proc_readlink(path, buf, bufsiz);
    }
    uint32_t ino = 0;
    int ft = 0;
    if (fs_lookup_ftype(path, &ino, &ft, 0) || ft != FT_SYMLINK) {
        current->errno = 22;
        return -1;
    }
    char kbuf[MAX_PATH_LEN];
    int len = fs_read_link_target(ino, kbuf, MAX_PATH_LEN);
    if (len < 0) {
        current->errno = 22;
        return -1;
    }
    uint32_t n = (uint32_t)len < bufsiz ? (uint32_t)len : bufsiz;
    memcpy(buf, kbuf, n);
    return (int32_t)n;
}

int32_t sys_access(const char *path, int32_t mode) {
    if (path == NULL) {
        return -1;
    }
    if (proc_match(path)) {
        return 0;
    }
    uint32_t ino_no = 0;
    int ft = 0;
    if (fs_lookup_ftype(path, &ino_no, &ft, 1)) {
        current->errno = 2;
        return -1;
    }
    struct FS_INODE obj;
    if (fs_read_inode(ino_no, &obj)) {
        return -1;
    }
    uint32_t bits = (uint32_t)mode & 7u;
    if (fs_check_perm(&obj, bits)) {
        current->errno = 13;
        return -1;
    }
    return 0;
}

int32_t sys_rename(const char *oldpath, const char *newpath) {
    return fs_rename_path(oldpath, newpath);
}

int32_t sys_truncate(const char *path, int32_t length) {
    if (length < 0) {
        current->errno = 22;
        return -1;
    }
    return fs_truncate_path(path, (uint32_t)length);
}

int32_t sys_chmod(const char *path, uint32_t mode) {
    if (path == NULL) {
        return -1;
    }
    if (proc_match(path)) {
        return 0;
    }
    uint32_t ino_no = 0;
    int ft = 0;
    if (fs_lookup_ftype(path, &ino_no, &ft, 1)) {
        current->errno = 2;
        return -1;
    }
    struct FS_INODE obj;
    if (fs_read_inode(ino_no, &obj)) {
        return -1;
    }
    if (current->euid != 0 && current->euid != obj.i_uid) {
        current->errno = 1;
        return -1;
    }
    obj.i_mode = (obj.i_mode & 0xF000u) | (mode & 0x0FFFu);
    return fs_write_inode(ino_no, &obj) ? -1 : 0;
}
