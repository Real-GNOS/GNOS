#include "kernel/syscall/linux_compat.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/rtc.h"
#include "drivers/char/tty.h"
#include "drivers/char/pty.h"
#include "drivers/net/socket.h"
#include "kernel/asm_func.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/fsapi.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/proc.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/shell/pipe.h"
#include "kernel/signal.h"
#include "kernel/syscall/file_syscall.h"
#include "kernel/syscall/futex.h"
#include "kernel/syscall/mmap.h"
#include "kernel/userprog/clone.h"
#include "kernel/userprog/exec.h"
#include "kernel/userprog/fork.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/rand/rand.h"
#include "lib/str/str.h"
#include "libc/user/syscall.h"
#include "kernel/syscall/lc_internal.h"

int32_t compat_read(int32_t fd, void *buf, uint32_t count);
int32_t compat_write(int32_t fd, const void *buf, uint32_t count);

#define DIRF_FLAG 0xFFFEu

int32_t compat_dir_fd(const char *path) {
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(path, &ino, &is_dir) || !is_dir)
        return -1;
    int gfd = file_table_alloc_slot();
    if (gfd < 0)
        return -1;
    struct FILE *f = file_get((uint32_t)gfd);
    f->fd_inode = inode_open(cur_part, ino);
    if (f->fd_inode == NULL) {
        file_table_free_slot(gfd);
        return -1;
    }
    f->fd_pos = 0;
    f->fd_flag = DIRF_FLAG;
    f->proc_id = 0;
    f->ref_cnt = 1;
    int32_t fd = fd_install(gfd);
    if (fd < 0) {
        inode_close(f->fd_inode);
        file_table_free_slot(gfd);
        return -1;
    }
    return fd;
}
int compat_fd_isdir(int32_t fd) {
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return 0;
    uint32_t gfd = fd_local2global((uint32_t)fd);
    if (gfd >= MAX_FILE_OPEN)
        return 0;
    struct FILE *pf = file_get(gfd);
    return pf != NULL && pf->fd_flag == DIRF_FLAG;
}

static int at_dir_inode(int32_t fd, uint32_t *out) {
    if (!compat_fd_isdir(fd))
        return -LINUX_EBADF;
    struct FILE *pf = file_get(fd_local2global((uint32_t)fd));
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    *out = pf->fd_inode->i_no;
    return 0;
}

int lc_at_path(struct ARCH_REGS *r, int32_t dirfd, uint64_t uptr, char *out) {
    if (!copy_user_str(r, out, uptr))
        return -LINUX_EFAULT;
    if (dirfd == LINUX_AT_FDCWD || out[0] == '/')
        return 0;
    uint32_t ino = 0;
    int rc = at_dir_inode(dirfd, &ino);
    if (rc != 0)
        return rc;
    char base[MAX_PATH_LEN];
    if (fs_inode_abs_path(ino, base, sizeof(base)) != 0)
        return -LINUX_ENOTDIR;
    uint32_t blen = (uint32_t)strlen(base);
    uint32_t rlen = (uint32_t)strlen(out);
    if (blen + rlen + 2 > (uint32_t)sizeof(base))
        return -LINUX_ENAMETOOLONG;
    if (blen > 1)
        base[blen++] = '/';
    memcpy(base + blen, out, rlen + 1);
    strcpy(out, base);
    return 0;
}
int32_t compat_getdents64(int32_t fd, void *dirp, uint32_t count) {
    if (dirp == NULL || fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EBADF;
    if (!compat_fd_isdir(fd))
        return -LINUX_ENOTDIR;
    uint32_t gfd = fd_local2global((uint32_t)fd);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL)
        return -LINUX_EBADF;
    if (pf->proc_id != 0)
        return proc_getdents64(pf, dirp, count);
    if (pf->fd_inode == NULL)
        return -LINUX_EBADF;
    uint32_t pos = pf->fd_pos;
    uint32_t emitted = pos;
    uint32_t written = 0;
    struct FS_DIRENT de;
    for (;;) {
        if (fs_dir_next(pf->fd_inode, &pos, &de) != 0)
            break;
        uint32_t nl = strlen(de.filename);
        uint16_t reclen = (uint16_t)((19u + nl + 1u + 7u) & ~7u);
        if (written + reclen > count) {
            if (written == 0)
                return -LINUX_EINVAL;
            break;
        }
        struct LINUX_DIRENT64 *d =
            (struct LINUX_DIRENT64 *)((uint8_t *)dirp + written);
        d->d_ino = de.i_no;
        d->d_off = (int64_t)pos;
        d->d_reclen = reclen;
        d->d_type = de.f_type == FT_DIRECTORY    ? LINUX_DT_DIR
                    : de.f_type == FT_CHARDEVICE ? LINUX_DT_CHR
                    : de.f_type == FT_SYMLINK    ? LINUX_DT_LNK
                                                 : LINUX_DT_REG;
        memcpy(d->d_name, de.filename, nl + 1);
        written += reclen;
        emitted = pos;
    }
    pf->fd_pos = emitted;
    return (int32_t)written;
}
int compat_fd_is_tty(int32_t fd) {
    if (fd >= 0 && fd <= 2)
        return 1;
    if (fd < 0)
        return 0;
    struct FILE *f = file_get(fd_local2global((uint32_t)fd));
    return f != NULL && f->fd_inode != NULL && fs_is_chardev(f->fd_inode) &&
           (fs_chardev_dev(f->fd_inode) >> 8) == 5u;
}
void compat_tcgets(uint8_t *p) {
    TTY.ioctl(TTY_IOCTL_TCGETS, (uint64_t)(uintptr_t)p);
}
int32_t compat_tcsets(uint32_t cmd, uint64_t arg) {
    (void)cmd;
    if (!arg || !access_ok((const void *)(uintptr_t)arg, 60, 0))
        return -LINUX_EFAULT;
    TTY.ioctl(TTY_IOCTL_TCSETS, arg);
    return 0;
}
int32_t compat_ioctl(int32_t fd, uint32_t cmd, uint64_t arg) {
    if (cmd == LINUX_FIONBIO) {
        int32_t nb = 0;
        int32_t rc;
        if (arg == 0 || !access_ok((const void *)(uintptr_t)arg, 4, 0))
            return -LINUX_EFAULT;
        memcpy(&nb, (const void *)(uintptr_t)arg, 4);
        rc = sys_fcntl(fd, LINUX_F_SETFL, nb ? LINUX_O_NONBLOCK : 0);
        return rc == 0 ? 0 : -LINUX_EBADF;
    }
    if (fd >= 0 && fd < MAX_FILES_OPEN_PER_PROC) {
        struct FILE *pf = file_get(fd_local2global((uint32_t)fd));
        if (pf != NULL && pf->dev_priv != NULL)
            return pty_chardev_ioctl(pf, cmd, arg);
    }
    if (fd >= 0 && compat_fd_is_tty(fd)) {
        switch (cmd) {
        case LINUX_TCGETS:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 60, 1))
                return -LINUX_EFAULT;
            compat_tcgets((uint8_t *)(uintptr_t)arg);
            return 0;
        case LINUX_TCSETS:
        case LINUX_TCSETSW:
        case LINUX_TCSETSF:
            return compat_tcsets(cmd, arg);
        case LINUX_TIOCGWINSZ:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 8, 1))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCGWINSZ, arg) < 0 ? -LINUX_ENOTTY
                                                            : 0;
        case LINUX_TIOCSWINSZ:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 8, 0))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCSWINSZ, arg) < 0 ? -LINUX_ENOTTY
                                                            : 0;
        case LINUX_TIOCGPGRP:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 4, 1))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCGPGRP, arg) < 0 ? -LINUX_ENOTTY : 0;
        case LINUX_TIOCSPGRP:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 4, 0))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_TIOCSPGRP, arg) < 0 ? -LINUX_ENOTTY : 0;
        case LINUX_TIOCSCTTY:
            return TTY.ioctl(TTY_IOCTL_TIOCSCTTY, arg) < 0 ? -LINUX_ENOTTY : 0;
        case LINUX_FIONREAD:
            if (!arg || !access_ok((const void *)(uintptr_t)arg, 4, 1))
                return -LINUX_EFAULT;
            return TTY.ioctl(TTY_IOCTL_FIONREAD, arg) < 0 ? -LINUX_ENOTTY : 0;
        case LINUX_TCFLSH:
            return 0;
        default:
            return -LINUX_ENOTTY;
        }
    }
    return -LINUX_ENOTTY;
}

int32_t compat_statfs_fill(uint64_t buf) {
    struct LINUX_STATFS sf;
    uint32_t bsize, blocks, bfree, files, ffree;
    fs_statfs_info(&bsize, &blocks, &bfree, &files, &ffree);
    memset(&sf, 0, sizeof(sf));
    sf.f_type = (int64_t)LINUX_EXT2_SUPER_MAGIC;
    sf.f_bsize = bsize;
    sf.f_blocks = blocks;
    sf.f_bfree = bfree;
    sf.f_bavail = bfree;
    sf.f_files = files;
    sf.f_ffree = ffree;
    sf.f_namelen = 255;
    sf.f_frsize = bsize;
    memcpy((void *)(uintptr_t)buf, &sf, sizeof(sf));
    return 0;
}

int64_t lc_statfs(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    if (strcmp(kpath, "/") != 0 && fs_lookup(kpath, &(uint32_t){0}, &(int){0}))
        return -LINUX_ENOENT;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_STATFS), 1))
        return -LINUX_EFAULT;
    return compat_statfs_fill(b);
}
int64_t lc_fstatfs(LC_ARGS) {
    (void)a;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_STATFS), 1))
        return -LINUX_EFAULT;
    return compat_statfs_fill(b);
}

int64_t lc_chown(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chown(kpath, (uint32_t)b, (uint32_t)c);
}
int64_t lc_lchown(LC_ARGS) {
    return lc_chown(r, a, b, c, d, e, f);
}
int64_t lc_fchown(LC_ARGS) {
    (void)r;
    if (a < 3 || a >= MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EBADF;
    uint32_t gfd = fd_local2global((uint32_t)a);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    struct FS_INODE obj;
    if (fs_read_inode(pf->fd_inode->i_no, &obj))
        return -LINUX_EIO;
    if (b != (uint64_t)-1)
        obj.i_uid = (uint16_t)b;
    if (c != (uint64_t)-1)
        obj.i_gid = (uint16_t)c;
    return fs_write_inode(pf->fd_inode->i_no, &obj) ? -LINUX_EIO : 0;
}
int64_t lc_fchownat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return sys_chown(kpath, (uint32_t)c, (uint32_t)d);
}
int64_t lc_fchmod(LC_ARGS) {
    (void)r;
    if (a < 3 || a >= MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EBADF;
    uint32_t gfd = fd_local2global((uint32_t)a);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    struct FS_INODE obj;
    if (fs_read_inode(pf->fd_inode->i_no, &obj))
        return -LINUX_EIO;
    obj.i_mode = (obj.i_mode & 0xF000u) | ((uint32_t)b & 0x0FFFu);
    return fs_write_inode(pf->fd_inode->i_no, &obj) ? -LINUX_EIO : 0;
}
int64_t lc_fchmodat(LC_ARGS) {
    (void)d;
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return sys_chmod(kpath, (uint32_t)c);
}
int64_t lc_close(LC_ARGS) {
    (void)r;
    int uslot = unix_fd_slot(a);
    if (uslot >= 0) {
        unix_close_slot(uslot);
        return 0;
    }
    if (lc_close_extra((int32_t)a) != 0)
        return 0;
    return close_file((int32_t)a);
}

int32_t compat_readv(int32_t fd, struct LINUX_IOVEC *iov,
                            int32_t iovcnt) {
    if (iovcnt < 0 || iovcnt > 16)
        return -LINUX_EINVAL;
    int32_t total = 0;
    for (int32_t i = 0; i < iovcnt; i++) {
        struct LINUX_IOVEC v;
        memcpy(&v, (const void *)(uintptr_t)&iov[i], sizeof(v));
        if (v.iov_len == 0)
            continue;
        int32_t n = compat_read(fd, v.iov_base, (uint32_t)v.iov_len);
        if (n < 0)
            return total > 0 ? total : n;
        total += n;
        if ((uint32_t)n < v.iov_len)
            break;
    }
    return total;
}

int32_t compat_ftruncate(int32_t fd, int32_t length) {
    (void)length;
    if (fd < 3 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return -LINUX_EINVAL;
    uint32_t gfd = fd_local2global((uint32_t)fd);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL || is_pipe((uint32_t)fd))
        return -LINUX_EINVAL;
    fs_truncate_inode(pf->fd_inode);
    fs_write_inode(pf->fd_inode->i_no, pf->fd_inode);
    return 0;
}
int32_t compat_flags_linux2native(uint32_t lflags) {
    int32_t nflags = (int32_t)(lflags & 3u);
    if (lflags & LINUX_O_CREAT)
        nflags |= O_CREAT;
    return nflags;
}
void compat_stat_fill(struct LINUX_STAT *ls, uint32_t ino, int64_t size,
                             uint32_t mode, uint32_t uid, uint32_t gid) {
    memset(ls, 0, sizeof(*ls));
    ls->st_dev = 0x800u;
    ls->st_ino = ino;
    ls->st_nlink = 1;
    ls->st_mode = mode;
    ls->st_uid = uid;
    ls->st_gid = gid;
    ls->st_blksize = 512;
    ls->st_blocks = (int64_t)((size + 511) / 512);
    ls->st_size = size;
    ls->st_atim.tv_sec = (int64_t)(tick / PIT_HZ);
    ls->st_mtim = ls->st_atim;
    ls->st_ctim = ls->st_atim;
}
static void stat_fill_inode(struct LINUX_STAT *ls, uint32_t ino,
                            uint32_t mode) {
    struct FS_INODE obj;
    uint32_t now = (uint32_t)rtc_unix_time();
    ls->st_nlink = (mode & 0xF000u) == 0x4000u ? fs_dir_nlink(ino) : 1;
    if (fs_read_inode(ino, &obj) != 0) {
        ls->st_atim.tv_sec = now;
        ls->st_mtim.tv_sec = now;
        ls->st_ctim.tv_sec = now;
        return;
    }
    ls->st_atim.tv_sec = obj.i_atime ? (int64_t)obj.i_atime : (int64_t)now;
    ls->st_mtim.tv_sec = obj.i_mtime ? (int64_t)obj.i_mtime : (int64_t)now;
    ls->st_ctim.tv_sec = obj.i_ctime ? (int64_t)obj.i_ctime : (int64_t)now;
}
int32_t compat_stat_linux(const char *path, uint64_t ub, int follow) {
    struct LINUX_STAT ls;
    uint32_t ino = 0, size = 0, mode = 0, uid = 0, gid = 0;
    if (proc_match(path)) {
        compat_stat_fill(&ls, 2, 0, LINUX_S_IFREG | 0444u, 0, 0);
    } else if (fs_stat_full(path, &ino, &size, &mode, &uid, &gid, follow) != 0) {
        return -LINUX_ENOENT;
    } else {
        compat_stat_fill(&ls, ino, (int64_t)size, mode, uid, gid);
        stat_fill_inode(&ls, ino, mode);
    }
    memcpy((void *)(uintptr_t)ub, &ls, sizeof(ls));
    return 0;
}
int32_t compat_fstat_linux(int32_t fd, uint64_t ub) {
    struct LINUX_STAT ls;
    if (fd >= 0 && fd < 3) {
        compat_stat_fill(&ls, 0, 0, LINUX_S_IFCHR | 0600u, 0, 0);
    } else if (compat_fd_isdir(fd)) {
        uint32_t gfd = fd_local2global((uint32_t)fd);
        struct FILE *pf = file_get(gfd);
        compat_stat_fill(&ls, pf->fd_inode->i_no, (int64_t)pf->fd_inode->i_size,
                         pf->fd_inode->i_mode, pf->fd_inode->i_uid,
                         pf->fd_inode->i_gid);
        stat_fill_inode(&ls, pf->fd_inode->i_no, pf->fd_inode->i_mode);
    } else if (is_pipe(fd)) {
        compat_stat_fill(&ls, 0, 0, LINUX_S_IFIFO | 0600u, 0, 0);
    } else {
        uint32_t gfd = fd_local2global((uint32_t)fd);
        struct FILE *pf = file_get(gfd);
        if (pf == NULL || pf->fd_inode == NULL)
            return -LINUX_EBADF;
        compat_stat_fill(&ls, pf->fd_inode->i_no,
                         (int64_t)pf->fd_inode->i_size, pf->fd_inode->i_mode,
                         pf->fd_inode->i_uid, pf->fd_inode->i_gid);
        stat_fill_inode(&ls, pf->fd_inode->i_no, pf->fd_inode->i_mode);
    }
    memcpy((void *)(uintptr_t)ub, &ls, sizeof(ls));
    return 0;
}
int32_t compat_openat(int32_t dirfd, const char *kpath, uint32_t lflags,
                      uint32_t mode) {
    if (dirfd != LINUX_AT_FDCWD)
        return -LINUX_EINVAL;
    struct FS_STAT pst;
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(kpath, &ino, &is_dir) == 0 && is_dir) {
        if (lflags & (LINUX_O_CREAT | LINUX_O_TRUNC | LINUX_O_APPEND))
            return -LINUX_EISDIR;
        int32_t fd = compat_dir_fd(kpath);
        return fd < 0 ? -LINUX_ENOENT : fd;
    }
    if (lflags & LINUX_O_DIRECTORY) {
        if (sys_stat(kpath, &pst) != 0 || pst.st_filetype != FT_DIRECTORY)
            return -LINUX_ENOTDIR;
    }
    if ((lflags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
        (LINUX_O_CREAT | LINUX_O_EXCL)) {
        if (sys_stat(kpath, &pst) == 0)
            return -LINUX_EEXIST;
    }
    int32_t fd = open_file_mode(kpath,
                                (uint8_t)compat_flags_linux2native(lflags),
                                mode & 0o7777u & ~current->umask);
    if (fd < 0)
        return -(current->errno > 0 ? current->errno : LINUX_ENOENT);
    if (lflags & LINUX_O_TRUNC)
        compat_ftruncate(fd, 0);
    if (lflags & LINUX_O_APPEND)
        sys_lseek(fd, 0, (uint8_t)SEEK_END);
    return fd;
}

int32_t compat_write(int32_t fd, const void *buf, uint32_t count) {
    if (fd < 0)
        return -1;
    if (fd < MAX_FILES_OPEN_PER_PROC) {
        struct FILE *pf = file_get(fd_local2global((uint32_t)fd));
        if (pf != NULL && pf->dev_priv != NULL)
            return (int32_t)pty_chardev_write(pf, buf, count);
    }
    int xi = evfd_slot(fd);
    if (xi >= 0)
        return (int32_t)lc_eventfd_write(xi, buf, count);
    xi = unix_fd_slot((uint64_t)fd);
    if (xi >= 0)
        return unix_send(xi, buf, count);
    if (tfd_slot(fd) >= 0 || ep_slot(fd) >= 0)
        return -LINUX_EINVAL;
    if (net_is_socket(fd)) {
        int32_t n = (int32_t)net_send(fd, buf, count);
        return n < 0 ? -LINUX_EAGAIN : n;
    }
    if (compat_fd_isdir(fd))
        return -LINUX_EISDIR;
    if (io_is_file_fd(fd)) {
        if (is_pipe(fd)) {
            struct FILE *pf2 = file_get(fd_local2global((uint32_t)fd));
            uint32_t n;
            if (pf2 == NULL || pf2->fd_inode == NULL)
                return -LINUX_EBADF;
            if (pf2->fd_flag != PIPE_FLAG)
                return -LINUX_EBADF;
            if (!pipe_end_alive(pf2->proc_aux))
                return -LINUX_EPIPE;
            if (pf2->fd_nonblock &&
                ioq_length((struct TTY_IOQUEUE *)pf2->fd_inode) >=
                    BUFSIZE - 1)
                return -LINUX_EAGAIN;
            n = pipe_write(fd, buf, count);
            if (n == (uint32_t)-1)
                return -LINUX_EPIPE;
            return (int32_t)n;
        }
        return (int32_t)write_file(fd, buf, count);
    }
    if (fd == 1 || fd == 2)
        return TTY.write((const char *)buf, count);
    const char *s = (const char *)buf;
    for (uint32_t k = 0; k < count; k++) {
        console_putc(s[k]);
    }
    return (int32_t)count;
}
int32_t compat_read(int32_t fd, void *buf, uint32_t count) {
    if (fd >= 0 && fd < MAX_FILES_OPEN_PER_PROC) {
        struct FILE *pf = file_get(fd_local2global((uint32_t)fd));
        if (pf != NULL && pf->dev_priv != NULL)
            return (int32_t)pty_chardev_read(pf, buf, count);
    }
    int xi = evfd_slot(fd);
    if (xi >= 0)
        return (int32_t)lc_eventfd_read(xi, buf, count);
    xi = tfd_slot(fd);
    if (xi >= 0)
        return (int32_t)lc_timerfd_read(xi, buf, count);
    xi = unix_fd_slot((uint64_t)fd);
    if (xi >= 0)
        return unix_recv(xi, buf, count);
    if (ep_slot(fd) >= 0)
        return -LINUX_EINVAL;
    if (net_is_socket(fd)) {
        int32_t n = (int32_t)net_recv(fd, buf, count);
        return n < 0 ? -LINUX_EAGAIN : n;
    }
    if (io_is_file_fd(fd)) {
        if (compat_fd_isdir(fd))
            return -LINUX_EISDIR;
        if (is_pipe(fd)) {
            struct FILE *pf3 = file_get(fd_local2global((uint32_t)fd));
            uint32_t len;
            if (pf3 == NULL || pf3->fd_inode == NULL)
                return -LINUX_EBADF;
            if (pf3->fd_flag != PIPE_RD_FLAG)
                return -LINUX_EBADF;
            len = ioq_length((struct TTY_IOQUEUE *)pf3->fd_inode);
            if (len == 0) {
                if (!pipe_end_alive(pf3->proc_aux))
                    return 0;
                if (pf3->fd_nonblock)
                    return -LINUX_EAGAIN;
                if (count == 0)
                    return 0;
                while (len == 0) {
                    if (!pipe_end_alive(pf3->proc_aux))
                        return 0;
                    mtime_sleep(1);
                    len = ioq_length((struct TTY_IOQUEUE *)pf3->fd_inode);
                }
            }
            return (int32_t)pipe_read(fd, buf, count);
        }
        return (int32_t)read_file(fd, buf, count);
    }
    if (fd == 0)
        return TTY.read((char *)buf, count);
    return -LINUX_EBADF;
}

int32_t sys_compat_writev(int32_t fd, struct LINUX_IOVEC *iov,
                                 int32_t iovcnt) {
    if (iovcnt < 0)
        return -1;
    uint32_t total = 0;
    for (int32_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        int32_t n =
            compat_write(fd, (const void *)iov[i].iov_base, iov[i].iov_len);
        if (n < 0)
            return -1;
        total += (uint32_t)n;
    }
    return (int32_t)total;
}

int64_t lc_write(LC_ARGS) {
    (void)d;
    (void)e;
    (void)f;
    if (!user_ptr_ok(r, b, (uint32_t)c, 0))
        return -LINUX_EFAULT;
    return compat_write((int32_t)a, (const void *)b, (uint32_t)c);
}
int64_t lc_read(LC_ARGS) {
    (void)d;
    (void)e;
    (void)f;
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return compat_read((int32_t)a, (void *)b, (uint32_t)c);
}
int64_t lc_pread64(LC_ARGS) {
    int32_t fd = (int32_t)a;
    int32_t saved;
    int32_t n;

    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    if (fd < 0 || (uint32_t)d > 0x7fffffffu)
        return -LINUX_EINVAL;
    saved = sys_lseek(fd, 0, SEEK_CUR);
    if (saved < 0)
        return -LINUX_ESPIPE;
    if (sys_lseek(fd, (int32_t)d, SEEK_SET) < 0)
        return -LINUX_EINVAL;
    n = compat_read(fd, (void *)b, (uint32_t)c);
    sys_lseek(fd, saved, SEEK_SET);
    return n;
}

int64_t lc_copy_file_range(LC_ARGS) {
    int32_t fd_in = (int32_t)a;
    int32_t fd_out = (int32_t)c;
    uint64_t remaining = e;
    int32_t saved_in = -1;
    int32_t saved_out = -1;
    int64_t off_in = 0;
    int64_t off_out = 0;
    int64_t copied = 0;
    uint8_t *buf;

    if (f != 0)
        return -LINUX_EINVAL;
    if (!io_is_file_fd(fd_in) || !io_is_file_fd(fd_out) ||
        compat_fd_isdir(fd_in) || compat_fd_isdir(fd_out))
        return -LINUX_EBADF;
    if (b != 0) {
        if (!user_ptr_ok(r, b, sizeof(off_in), 1))
            return -LINUX_EFAULT;
        memcpy(&off_in, (const void *)(uintptr_t)b, sizeof(off_in));
        if (off_in < 0 || off_in > 0x7fffffffll)
            return -LINUX_EINVAL;
        saved_in = sys_lseek(fd_in, 0, SEEK_CUR);
        if (saved_in < 0 || sys_lseek(fd_in, (int32_t)off_in, SEEK_SET) < 0)
            return -LINUX_EINVAL;
    }
    if (d != 0) {
        if (!user_ptr_ok(r, d, sizeof(off_out), 1)) {
            if (saved_in >= 0)
                sys_lseek(fd_in, saved_in, SEEK_SET);
            return -LINUX_EFAULT;
        }
        memcpy(&off_out, (const void *)(uintptr_t)d, sizeof(off_out));
        if (off_out < 0 || off_out > 0x7fffffffll) {
            if (saved_in >= 0)
                sys_lseek(fd_in, saved_in, SEEK_SET);
            return -LINUX_EINVAL;
        }
        saved_out = sys_lseek(fd_out, 0, SEEK_CUR);
        if (saved_out < 0 ||
            sys_lseek(fd_out, (int32_t)off_out, SEEK_SET) < 0) {
            if (saved_in >= 0)
                sys_lseek(fd_in, saved_in, SEEK_SET);
            return -LINUX_EINVAL;
        }
    }

    buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        if (saved_in >= 0)
            sys_lseek(fd_in, saved_in, SEEK_SET);
        if (saved_out >= 0)
            sys_lseek(fd_out, saved_out, SEEK_SET);
        return -LINUX_ENOMEM;
    }
    while (remaining != 0) {
        uint32_t want = remaining > PAGE_SIZE ? PAGE_SIZE : (uint32_t)remaining;
        int32_t nr = compat_read(fd_in, buf, want);
        if (nr <= 0) {
            if (nr < 0 && copied == 0)
                copied = nr;
            break;
        }
        uint32_t done = 0;
        while (done < (uint32_t)nr) {
            int32_t nw = compat_write(fd_out, buf + done, (uint32_t)nr - done);
            if (nw <= 0) {
                if (copied == 0)
                    copied = nw < 0 ? nw : -LINUX_EIO;
                remaining = 0;
                break;
            }
            done += (uint32_t)nw;
            copied += nw;
            remaining -= (uint32_t)nw;
        }
        if (done < (uint32_t)nr)
            break;
    }
    free_kernel_page((uint32_t)(uintptr_t)buf);

    if (b != 0) {
        off_in += copied > 0 ? copied : 0;
        memcpy((void *)(uintptr_t)b, &off_in, sizeof(off_in));
        sys_lseek(fd_in, saved_in, SEEK_SET);
    }
    if (d != 0) {
        off_out += copied > 0 ? copied : 0;
        memcpy((void *)(uintptr_t)d, &off_out, sizeof(off_out));
        sys_lseek(fd_out, saved_out, SEEK_SET);
    }
    return copied;
}

int64_t lc_writev(LC_ARGS) {
    (void)d;
    (void)e;
    (void)f;
    if (!user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    return sys_compat_writev((int32_t)a, (struct LINUX_IOVEC *)b, (int32_t)c);
}
int64_t lc0_writev(LC_ARGS) {
    if ((b == 0 && c > 0) || c > 1024 ||
        !user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    int32_t total = 0;
    for (int32_t i = 0; i < (int32_t)c; i++) {
        uint32_t pair[2];
        memcpy(pair, (const void *)(uintptr_t)(b + (uint32_t)i * 8u),
               sizeof(pair));
        if (pair[1] == 0)
            continue;
        if (!user_ptr_ok(r, pair[0], pair[1], 0))
            return -LINUX_EFAULT;
        int32_t n =
            compat_write((int32_t)a, (const void *)(uintptr_t)pair[0], pair[1]);
        if (n < 0)
            return n;
        total += n;
        if ((uint32_t)n < pair[1])
            break;
    }
    return total;
}
int64_t lc_fstat(LC_ARGS) {
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    return compat_fstat_linux((int32_t)a, b);
}
int64_t lc_stat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) ||
        !user_ptr_ok(r, b, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    return compat_stat_linux(kpath, b, 1);
}
int64_t lc_lstat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) ||
        !user_ptr_ok(r, b, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    return compat_stat_linux(kpath, b, 0);
}
int64_t lc_lseek(LC_ARGS) {
    (void)r;
    return sys_lseek((int32_t)a, (int32_t)b, (uint8_t)(c + 1u));
}
int64_t lc_fcntl(LC_ARGS) {
    (void)r;
    return sys_fcntl((int32_t)a, (int32_t)b, c);
}
#define FLOCK_TAB_N 16
static struct {
    uint32_t ino;
    uint32_t mode;
    int32_t pid;
} flock_tab[FLOCK_TAB_N];
static int flock_conflict(uint32_t ino, uint32_t want, int32_t pid) {
    for (uint32_t i = 0; i < FLOCK_TAB_N; i++) {
        if (flock_tab[i].mode != 0 && flock_tab[i].ino == ino &&
            flock_tab[i].pid != pid &&
            (flock_tab[i].mode == 2u || want == 2u))
            return 1;
    }
    return 0;
}

void flock_release_ino(uint32_t ino) {
    uint32_t fl = asm_save_eflags();
    asm_cli();
    for (uint32_t i = 0; i < FLOCK_TAB_N; i++) {
        if (flock_tab[i].mode != 0 && flock_tab[i].ino == ino)
            flock_tab[i].mode = 0;
    }
    asm_restore_eflags(fl);
}
int64_t lc_flock(LC_ARGS) {
    (void)r;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    uint32_t gfd = fd_local2global((uint32_t)a);
    struct FILE *pf = file_get(gfd);
    if (pf == NULL || pf->fd_inode == NULL)
        return -LINUX_EBADF;
    uint32_t ino = pf->fd_inode->i_no;
    int32_t pid = (int32_t)current->fd_owner_pid;
    uint32_t op = (uint32_t)b & 0xfu;
    if (op == 8u) {
        uint32_t fl = asm_save_eflags();
        asm_cli();
        for (uint32_t i = 0; i < FLOCK_TAB_N; i++) {
            if (flock_tab[i].mode != 0 && flock_tab[i].ino == ino &&
                flock_tab[i].pid == pid)
                flock_tab[i].mode = 0;
        }
        asm_restore_eflags(fl);
        return 0;
    }
    if (op != 1u && op != 2u)
        return -LINUX_EINVAL;
    for (;;) {
        uint32_t fl = asm_save_eflags();
        asm_cli();
        if (!flock_conflict(ino, op, pid)) {
            for (uint32_t i = 0; i < FLOCK_TAB_N; i++) {
                if (flock_tab[i].mode != 0 && flock_tab[i].ino == ino &&
                    flock_tab[i].pid == pid) {
                    flock_tab[i].mode = op;
                    asm_restore_eflags(fl);
                    return 0;
                }
            }
            for (uint32_t i = 0; i < FLOCK_TAB_N; i++) {
                if (flock_tab[i].mode == 0) {
                    flock_tab[i].ino = ino;
                    flock_tab[i].mode = op;
                    flock_tab[i].pid = pid;
                    asm_restore_eflags(fl);
                    return 0;
                }
            }
            asm_restore_eflags(fl);
            return -LINUX_ENOLCK;
        }
        asm_restore_eflags(fl);
        if (b & 4u)
            return -LINUX_EWOULDBLOCK;
        thread_yield();
    }
}
int64_t lc_readlink(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) || !user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return sys_readlink(kpath, (char *)b, c);
}
int64_t lc_chdir(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chdir(kpath);
}
int64_t lc_getcwd(LC_ARGS) {
    if (!user_ptr_ok(r, a, (uint32_t)b, 1))
        return -LINUX_EFAULT;
    return sys_getcwd((char *)a, (uint32_t)b) ? (int64_t)a : -LINUX_ENOENT;
}
static void lc_parent_of(const char *path, char *out) {
    uint32_t n = (uint32_t)strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    while (n > 1 && path[n - 1] != '/')
        n--;
    while (n > 1 && path[n - 1] == '/')
        n--;
    if (n <= 1) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static int64_t lc_mkdir_apply(const char *kpath) {
    if (sys_mkdir(kpath) == 0)
        return 0;
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(kpath, &ino, &is_dir) == 0)
        return -LINUX_EEXIST;
    char parent[MAX_PATH_LEN];
    lc_parent_of(kpath, parent);
    if (fs_lookup(parent, &ino, &is_dir) != 0 || !is_dir)
        return -LINUX_ENOENT;
    return -LINUX_EPERM;
}

int64_t lc_mkdir(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return lc_mkdir_apply(kpath);
}
int64_t lc_rmdir(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_rmdir(kpath);
}
int64_t lc_unlink(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_unlink(kpath);
}
int64_t lc_rename(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    char kpath2[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a) || !copy_user_str(r, kpath2, b))
        return -LINUX_EFAULT;
    return sys_rename(kpath, kpath2);
}
int64_t lc_chmod(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_chmod(kpath, b);
}
int64_t lc_mknod(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_mknod(kpath, (uint32_t)b, (uint32_t)c);
}
int64_t lc_symlink(LC_ARGS) {
    char ktarget[MAX_PATH_LEN];
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, ktarget, a) || !copy_user_str(r, kpath, b))
        return -LINUX_EFAULT;
    return sys_symlink(ktarget, kpath);
}
int64_t lc_symlinkat(LC_ARGS) {
    char ktarget[MAX_PATH_LEN];
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, ktarget, a))
        return -LINUX_EFAULT;
    int rc = lc_at_path(r, (int32_t)b, c, kpath);
    if (rc != 0)
        return rc;
    return sys_symlink(ktarget, kpath);
}
int64_t lc_mknodat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return sys_mknod(kpath, (uint32_t)c, (uint32_t)d);
}
int64_t lc_access(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_access(kpath, (int32_t)b);
}

int64_t lc_getdents64(LC_ARGS) {
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    return compat_getdents64((int32_t)a, (void *)(uintptr_t)b, (uint32_t)c);
}
int64_t lc_ioctl(LC_ARGS) {
    (void)r;
    return compat_ioctl((int32_t)a, (uint32_t)b, c);
}
int64_t lc_readv(LC_ARGS) {
    if (!user_ptr_ok(r, b, (uint32_t)c * 8u, 0))
        return -LINUX_EFAULT;
    return compat_readv((int32_t)a, (struct LINUX_IOVEC *)(uintptr_t)b,
                        (int32_t)c);
}

int64_t lc_ftruncate(LC_ARGS) {
    (void)r;
    return compat_ftruncate((int32_t)a, (int32_t)c);
}

int64_t lc_pipe(LC_ARGS) {
    if (a == 0 || !user_ptr_ok(r, a, 8, 1))
        return -LINUX_EFAULT;
    return sys_pipe((int32_t *)(uintptr_t)a);
}
int64_t lc_pipe2(LC_ARGS) {
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    uint32_t flags = (uint32_t)b;
    if ((flags & ~(LINUX_O_CLOEXEC | LINUX_O_NONBLOCK)) != 0)
        return -LINUX_EINVAL;
    int64_t rc = lc_pipe(r, a, 0, 0, 0, 0, 0);
    if (rc < 0)
        return rc;
    if (flags == 0)
        return 0;
    int32_t fds[2];
    if (!user_ptr_ok(r, a, 8, 0))
        return -LINUX_EFAULT;
    memcpy(fds, (const void *)(uintptr_t)a, sizeof(fds));
    for (int k = 0; k < 2; k++) {
        if (fds[k] < 0 || fds[k] >= (int32_t)MAX_FILES_OPEN_PER_PROC)
            continue;
        if ((flags & LINUX_O_CLOEXEC) != 0)
            current->fd_cloexec |= (1ull << fds[k]);
        if ((flags & LINUX_O_NONBLOCK) != 0) {
            struct FILE *pf = file_get(fd_local2global((uint32_t)fds[k]));
            if (pf != NULL)
                pf->fd_nonblock = 1;
        }
    }
    return 0;
}
int64_t lc_dup(LC_ARGS) {
    (void)r;
    return sys_dup((int32_t)a);
}
int64_t lc_dup2(LC_ARGS) {
    (void)r;
    return sys_dup2((int32_t)a, (int32_t)b);
}
int64_t lc_dup3(LC_ARGS) {
    struct TASK *fd_task = fd_owner_task();
    if ((int32_t)a == (int32_t)b ||
        (c & ~(uint64_t)LINUX_O_CLOEXEC) != 0)
        return -LINUX_EINVAL;
    int32_t newfd = sys_dup2((int32_t)a, (int32_t)b);
    if (newfd < 0)
        return -LINUX_EBADF;
    if ((c & LINUX_O_CLOEXEC) != 0)
        fd_task->fd_cloexec |= 1ull << (uint32_t)newfd;
    else
        fd_task->fd_cloexec &= ~(1ull << (uint32_t)newfd);
    return newfd;
}
int64_t lc_open(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return compat_openat(LINUX_AT_FDCWD, kpath, (uint32_t)b, (uint32_t)c);
}
int64_t lc_openat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return compat_openat(LINUX_AT_FDCWD, kpath, (uint32_t)c, (uint32_t)d);
}
int64_t lc_newfstatat(LC_ARGS) {
    if (!user_ptr_ok(r, c, sizeof(struct LINUX_STAT), 1))
        return -LINUX_EFAULT;
    if ((b == 0 || *(const char *)(uintptr_t)b == 0) &&
        (d & LINUX_AT_EMPTY_PATH))
        return compat_fstat_linux((int32_t)a, c);
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return compat_stat_linux(kpath, c,
                             (d & LINUX_AT_SYMLINK_NOFOLLOW) ? 0 : 1);
}
int64_t lc_unlinkat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return (d & LINUX_AT_REMOVEDIR) ? sys_rmdir(kpath) : sys_unlink(kpath);
}
int64_t lc_mkdirat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return lc_mkdir_apply(kpath);
}
int64_t lc_renameat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    char kpath2[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc == 0)
        rc = lc_at_path(r, (int32_t)c, d, kpath2);
    if (rc != 0)
        return rc;
    return sys_rename(kpath, kpath2);
}
int64_t lc_renameat2(LC_ARGS) {
    if (e != 0)
        return -LINUX_EINVAL;
    return lc_renameat(r, a, b, c, d, 0, 0);
}
int64_t lc_readlinkat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    if (!user_ptr_ok(r, c, (uint32_t)d, 1))
        return -LINUX_EFAULT;
    return sys_readlink(kpath, (char *)(uintptr_t)c, d);
}
int64_t lc_faccessat(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    int rc = lc_at_path(r, (int32_t)a, b, kpath);
    if (rc != 0)
        return rc;
    return sys_access(kpath, (int32_t)c);
}
int64_t lc_utimensat(LC_ARGS) {
    uint32_t now = (uint32_t)rtc_unix_time();
    uint32_t at = now;
    uint32_t mt = now;
    if (c != 0) {
        struct LINUX_TIMESPEC ts[2];
        if (!user_ptr_ok(r, c, sizeof(ts), 1))
            return -LINUX_EFAULT;
        memcpy(ts, (const void *)(uintptr_t)c, sizeof(ts));
        if (ts[0].tv_nsec != LINUX_UTIME_OMIT)
            at = ts[0].tv_nsec == LINUX_UTIME_NOW ? now
                                                  : (uint32_t)ts[0].tv_sec;
        if (ts[1].tv_nsec != LINUX_UTIME_OMIT)
            mt = ts[1].tv_nsec == LINUX_UTIME_NOW ? now
                                                  : (uint32_t)ts[1].tv_sec;
    }
    uint32_t ino = 0;
    if (b == 0 || *(const char *)(uintptr_t)b == 0) {
        if (a < 0 || a >= MAX_FILES_OPEN_PER_PROC)
            return -LINUX_EBADF;
        struct FILE *pf = file_get(fd_local2global((uint32_t)a));
        if (pf == NULL || pf->fd_inode == NULL)
            return -LINUX_EBADF;
        ino = pf->fd_inode->i_no;
    } else {
        char kpath[MAX_PATH_LEN];
        int rc = lc_at_path(r, (int32_t)a, b, kpath);
        if (rc != 0)
            return rc;
        int ft = 0;
        if (fs_lookup_ftype(kpath, &ino, &ft, 1) != 0)
            return -LINUX_ENOENT;
    }
    struct FS_INODE obj;
    if (fs_read_inode(ino, &obj))
        return -LINUX_EIO;
    if (current->euid != 0 && current->euid != obj.i_uid &&
        fs_check_perm(&obj, 2u)) {
        return -LINUX_EACCES;
    }
    obj.i_atime = at;
    obj.i_mtime = mt;
    obj.i_ctime = now;
    return fs_write_inode(ino, &obj) ? -LINUX_EIO : 0;
}

int64_t lc0_open(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return open_file(kpath, (uint8_t)b);
}

int64_t lc_fsync(LC_ARGS) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    if (evfd_slot((int)a) >= 0 || tfd_slot((int)a) >= 0 || ep_slot((int)a) >= 0)
        return -LINUX_EINVAL;
    if (a < 3)
        return 0;
    if (!io_is_file_fd((int)a))
        return -LINUX_EBADF;
    if (is_pipe((int)a))
        return -LINUX_EINVAL;
    return 0;
}
int64_t lc_truncate(LC_ARGS) {
    (void)c; (void)d; (void)e; (void)f;
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    if ((int64_t)b < 0)
        return -LINUX_EINVAL;
    return sys_truncate(kpath, (int32_t)b);
}
