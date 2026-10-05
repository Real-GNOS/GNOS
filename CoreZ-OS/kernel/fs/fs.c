#include "kernel/fs/fs.h"
#include "drivers/char/console/io.h"
#include "drivers/char/ioqueue.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/shell/pipe.h"
#include "kernel/sched/thread.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/fsapi.h"
#include "kernel/fs/file.h"
#include "drivers/char/tty.h"
#include "drivers/char/pty.h"
#include "kernel/fs/inode.h"
#include "kernel/fs/proc.h"
#include "kernel/syscall/lc_internal.h"
struct DISK_PARTITION *cur_part;
void filesys_init(void) {
    file_table_init();
    if (fs_init()) {
        kprintf("filesys: ext2 init failed\n");
        return;
    }
    cur_part = fs_partition();
    if (cur_part == NULL) {
        return;
    }
    rb_root_init(&cur_part->open_inodes_rb);
    open_root_dir(cur_part);
    kprintf("filesys init done, root=%s\n", cur_part->name);
}

char *path_parse(char *pathname, char *name_store) {
    uint32_t cnt = 0;
    if (pathname[0] == '/') {
        while (*(++pathname) == '/')
            ;
    }
    while (*pathname != '/' && *pathname != 0 && cnt < MAX_FILE_NAME_LEN - 1) {
        *name_store++ = *pathname++;
        cnt++;
    }
    if (pathname[0] == 0) {
        return NULL;
    }
    return pathname;
}

int search_file(const char *pathname) {
    if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") ||
        !strcmp(pathname, "/..")) {
        return 2;
    }
    if (pathname[0] != '/' || strlen(pathname) <= 1) {
        return -1;
    }
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(pathname, &ino, &is_dir)) {
        return -1;
    }
    return (int)ino;
}

static int fs_create_common(const char *pathname, uint32_t mode, int is_dir);
int create_file_mode(const char *pathname, uint32_t mode) {
    return fs_create_common(pathname, 0x8000u | (mode & 0o7777u), 0);
}
int create_file(const char *pathname) {
    return create_file_mode(pathname, 0o666);
}

static int split_parent_path(const char *pathname, char *parent, char *base,
                             uint32_t buf_len) {
    char abs[MAX_PATH_LEN];
    if (fs_abs_path(pathname, abs, sizeof(abs)) != 0) {
        return -1;
    }
    uint32_t plen = (uint32_t)strlen(abs);
    if (plen >= buf_len) {
        return -1;
    }
    memcpy(parent, abs, plen + 1);
    uint32_t i = plen;
    while (i > 1 && parent[i - 1] == '/') {
        parent[--i] = 0;
    }
    if (i == 0) {
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (slash == NULL) {
        return -1;
    }
    strcpy(base, slash + 1);
    if (base[0] == 0) {
        return -1;
    }
    *slash = 0;
    if (slash == parent) {
        parent[0] = '/';
        parent[1] = 0;
    }
    return 0;
}

static uint32_t get_parent_inode(const char *parent) {
    uint32_t pino = 0;
    int is_dir = 0;
    if (fs_lookup(parent, &pino, &is_dir) || !is_dir) {
        return 0;
    }
    return pino;
}

static void fs_free_best_effort(struct FS_INODE *ino);
static int fs_create_common(const char *pathname, uint32_t mode, int is_dir) {
    char parent[MAX_PATH_LEN];
    char base[MAX_PATH_LEN];
    if (split_parent_path(pathname, parent, base, MAX_PATH_LEN)) {
        return -1;
    }
    uint32_t pino = get_parent_inode(parent);
    if (pino == 0) {
        current->errno = 2;
        return -1;
    }
    if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        current->errno = 22;
        return -1;
    }
    uint32_t tino = 0;
    int tft = 0;
    if (fs_lookup_ftype(pathname, &tino, &tft, 0) == 0) {
        current->errno = 17;
        return -1;
    }
    struct FS_INODE par;
    if (fs_read_inode(pino, &par)) {
        kprintf("create: read inode fail\n");
        current->errno = 2;
        return -1;
    }
    if (fs_check_perm(&par, 2u)) {
        kprintf("create: perm deny\n");
        current->errno = 13;
        return -1;
    }
    struct FS_INODE newi;
    uint32_t ino = fs_new_inode(mode, &newi);
    if (ino == 0) {
        kprintf("create: no inode\n");
        current->errno = 28;
        return -1;
    }
    newi.i_uid = current->euid;
    newi.i_gid = current->egid;
    fs_write_inode(ino, &newi);
    if (is_dir) {
        if (fs_add_entry(&newi, ino, ".", 1) ||
            fs_add_entry(&newi, pino, "..", 1)) {
            fs_free_best_effort(&newi);
            return -1;
        }
    }
    uint32_t fmt = mode & 0xF000u;
    uint8_t dt = fmt == 0x4000u ? FS_DT_DIR
                : fmt == 0xA000u ? FS_DT_LNK
                : fmt == 0x2000u ? FS_DT_CHR
                                 : 1u;
    if (fs_add_entry_dt(&par, ino, base, dt)) {
        kprintf("create: add entry fail\n");
        fs_free_best_effort(&newi);
        current->errno = 28;
        return -1;
    }
    return (int)ino;
}

static void fs_free_best_effort(struct FS_INODE *ino) {
    fs_truncate_inode(ino);
    fs_write_inode(ino->i_no, ino);
    fs_free_inode(ino->i_no);
}

int fs_check_perm(const struct FS_INODE *ino, uint32_t bits) {
    struct TASK *cur = current;
    if (cur->euid == 0) {
        if ((bits & 1u) && !(ino->i_mode & 0111u))
            return -1;
        return 0;
    }
    uint32_t shift;
    if (cur->euid == ino->i_uid)
        shift = 6;
    else if (cur->egid == ino->i_gid)
        shift = 3;
    else
        shift = 0;
    if (((ino->i_mode >> shift) & 7u & bits) != bits)
        return -1;
    return 0;
}

int fs_is_chardev(const struct FS_INODE *ino) {
    return ino != NULL && (ino->i_mode & 0xF000u) == 0x2000u;
}

uint32_t fs_chardev_dev(const struct FS_INODE *ino) {
    return ino->i_block[0];
}

int32_t sys_symlink(const char *target, const char *linkpath) {
    if (target == NULL || linkpath == NULL) {
        return -1;
    }
    uint32_t tlen = (uint32_t)strlen(target);
    if (tlen == 0 || tlen >= MAX_PATH_LEN) {
        return -1;
    }
    uint32_t ino = 0;
    int ft = 0;
    if (fs_lookup_ftype(linkpath, &ino, &ft, 0) == 0) {
        return -1;
    }
    ino = (uint32_t)fs_create_common(linkpath, 0xA1FFu, 0);
    if ((int32_t)ino <= 0) {
        return -1;
    }
    struct FS_INODE node;
    if (fs_read_inode(ino, &node)) {
        return -1;
    }
    if (tlen < 60u) {
        memset(node.i_block, 0, sizeof(node.i_block));
        memcpy(&node.i_block[0], target, tlen);
        node.i_size = tlen;
    } else if (fs_write_to_inode(&node, 0, target, tlen) != (int)tlen) {
        fs_free_best_effort(&node);
        return -1;
    }
    return fs_write_inode(ino, &node) ? -1 : 0;
}

static int fs_stat_node(uint32_t ino_no, uint32_t *size, uint32_t *mode,
                        uint32_t *uid, uint32_t *gid) {
    struct FS_INODE obj;
    if (fs_read_inode(ino_no, &obj))
        return -1;
    if (size) *size = obj.i_size;
    if (mode) *mode = obj.i_mode;
    if (uid) *uid = obj.i_uid;
    if (gid) *gid = obj.i_gid;
    return 0;
}

int fs_stat_full(const char *path, uint32_t *ino_out, uint32_t *size,
                 uint32_t *mode, uint32_t *uid, uint32_t *gid, int follow) {
    uint32_t ino_no = 0;
    int ft = 0;
    if (fs_lookup_ftype(path, &ino_no, &ft, follow))
        return -1;
    if (fs_stat_node(ino_no, size, mode, uid, gid))
        return -1;
    if (mode) {
        *mode = (*mode & ~0xF000u) |
                (ft == FT_DIRECTORY    ? 0x4000u
                 : ft == FT_CHARDEVICE ? 0x2000u
                 : ft == FT_SYMLINK    ? 0xA000u
                                       : 0x8000u);
    }
    if (ino_out) *ino_out = ino_no;
    return 0;
}

int32_t sys_chown(const char *path, uint32_t uid, uint32_t gid) {
    uint32_t ino_no = 0;
    int ft = 0;
    if (path == NULL || fs_lookup_ftype(path, &ino_no, &ft, 1))
        return -1;
    struct FS_INODE obj;
    if (fs_read_inode(ino_no, &obj))
        return -1;
    if (current->euid != 0) {
        current->errno = 1;
        return -1;
    }
    if (uid != (uint32_t)-1)
        obj.i_uid = (uint16_t)uid;
    if (gid != (uint32_t)-1)
        obj.i_gid = (uint16_t)gid;
    return fs_write_inode(ino_no, &obj) ? -1 : 0;
}

int32_t sys_mknod(const char *path, uint32_t mode, uint32_t dev) {
    if (path == NULL || (mode & 0xF000u) != 0x2000u) {
        return -1;
    }
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(path, &ino, &is_dir) == 0) {
        return -1;
    }
    ino = (uint32_t)fs_create_common(path, mode, 0);
    if ((int32_t)ino <= 0) {
        return -1;
    }
    struct FS_INODE node;
    if (fs_read_inode(ino, &node)) {
        return -1;
    }
    node.i_block[0] = dev;
    return fs_write_inode(ino, &node) ? -1 : 0;
}

int open_file_mode(const char *pathname, uint8_t flags, uint32_t mode) {
    if (pathname == NULL || pathname[strlen(pathname) - 1] == '/') {
        return -1;
    }
    char altpath[32];
    if (strncmp(pathname, "/dev/pts/", 9) == 0) {
        const char *d = pathname + 9;
        int i = 0;
        for (; d[i] && i < 8; i++) {
            if (d[i] < '0' || d[i] > '9')
                break;
        }
        if (d[i] == '\0' && i > 0) {
            memcpy(altpath, "/dev/pty", 8);
            for (int j = 0; j < i; j++)
                altpath[8 + j] = d[j];
            altpath[8 + i] = '\0';
            pathname = altpath;
        }
    }
    if (proc_match(pathname)) {
        return proc_open(pathname, flags);
    }
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(pathname, &ino, &is_dir)) {
        if ((flags & O_CREAT) != 0) {
            if (create_file_mode(pathname, mode & ~current->umask) <= 0) {
                current->errno = 2;
                return -1;
            }
            if (fs_lookup(pathname, &ino, &is_dir) || is_dir) {
                return -1;
            }
        } else {
            return -1;
        }
    } else if (is_dir) {
        return -1;
    }
    int gfd = file_table_alloc_slot();
    if (gfd == -1) {
        return -1;
    }
    struct FILE *file = file_get((uint32_t)gfd);
    file->fd_pos = 0;
    file->fd_flag = flags;
    file->fd_inode = inode_open(cur_part, ino);
    if (file->fd_inode == NULL) {
        file_table_free_slot(gfd);
        return -1;
    }
    uint32_t low = flags & 3u;
    uint32_t need = (low == O_WRONLY) ? 2u : 4u;
    if (low == O_RDWR)
        need |= 2u;
    if (fs_check_perm(file->fd_inode, need)) {
        current->errno = 13;
        inode_close(file->fd_inode);
        file_table_free_slot(gfd);
        return -1;
    }
    file->ref_cnt = 1;
    if (fs_is_chardev(file->fd_inode)) {
        uint32_t dev = fs_chardev_dev(file->fd_inode);
        uint32_t major = dev >> 8;
        if (major == PTY_MASTER_MAJOR || major == PTY_SLAVE_MAJOR) {
            if (pty_chardev_open(file, dev) < 0) {
                inode_close(file->fd_inode);
                file_table_free_slot(gfd);
                current->errno = 16;
                return -1;
            }
        }
    }
    int fd = fd_install(gfd);
    if (fd == -1) {
        inode_close(file->fd_inode);
        file_table_free_slot(gfd);
        return -1;
    }
    return fd;
}

int open_file(const char *pathname, uint8_t flags) {
    return open_file_mode(pathname, flags, 0o666);
}

uint32_t fs_dir_nlink(uint32_t ino) {
    struct FS_INODE *dino = inode_open(cur_part, ino);
    if (dino == NULL) {
        return 2;
    }
    uint32_t pos = 0;
    uint32_t n = 2;
    struct FS_DIRENT de;
    while (fs_dir_next(dino, &pos, &de) == 0) {
        if (de.f_type == FT_DIRECTORY && strcmp(de.filename, ".") != 0 &&
            strcmp(de.filename, "..") != 0) {
            n++;
        }
    }
    inode_close(dino);
    return n;
}

int close_file(int fd) {
    if (fd < 3 || fd >= MAX_FILES_OPEN_PER_PROC)
        return -1;

    uint32_t global_fd_idx = fd_local2global((uint32_t)fd);
    if (global_fd_idx == (uint32_t)-1)
        return -1;

    current->pipe_wr_mask &= ~(1ull << (uint64_t)fd);
    fd_release((uint32_t)fd);
    if (global_fd_idx >= MAX_FILE_OPEN)
        return 0;

    struct FILE *file = file_get(global_fd_idx);
    if (file == NULL || file->ref_cnt == 0)
        return 0;

    if (file_table_unref(global_fd_idx) > 0)
        return 0;

    if (file->dev_priv)
        pty_chardev_close(file);

    if (file->fd_flag == PIPE_FLAG || file->fd_flag == PIPE_RD_FLAG) {
        struct TTY_IOQUEUE *ioq = (struct TTY_IOQUEUE *)file->fd_inode;
        if (ioq != NULL && ioq->ends != 0) {
            ioq->ends = ioq->ends - 1;
            if (ioq->ends == 0)
                free_kernel_page((uint32_t)ioq);
        }
    } else if (file->fd_inode != NULL) {
        flock_release_ino(file->fd_inode->i_no);
        inode_close(file->fd_inode);
    }

    file_table_free_slot((int)global_fd_idx);
    return 0;
}

uint32_t read_file(int fd, void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        return (uint32_t)-1;
    }
    uint32_t global_fd_idx = fd_local2global((uint32_t)fd);
    if (global_fd_idx == (uint32_t)-1) {
        return (uint32_t)-1;
    }
    struct FILE *pf = file_get(global_fd_idx);
    if (pf->proc_id != 0) {
        return proc_read(pf, buf, count);
    }
    return file_read(pf, buf, count);
}

uint32_t write_file(int fd, const void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        return (uint32_t)-1;
    }
    uint32_t global_fd_idx = fd_local2global((uint32_t)fd);
    if (global_fd_idx == (uint32_t)-1) {
        return (uint32_t)-1;
    }
    return file_write(file_get(global_fd_idx), buf, count);
}

#define LSEEK_ESPIPE 29
#define LSEEK_EBADF 9
#define LSEEK_EINVAL 22

int32_t sys_lseek(int32_t fd, int32_t offset, uint8_t whence) {
    if (fd < 3) {
        return -LSEEK_ESPIPE;
    }
    if (fd >= MAX_FILES_OPEN_PER_PROC) {
        return -LSEEK_EBADF;
    }
    uint32_t global_fd_idx = fd_local2global((uint32_t)fd);
    if (global_fd_idx == (uint32_t)-1) {
        return -LSEEK_EBADF;
    }
    struct FILE *pf = file_get(global_fd_idx);
    if (pf->proc_id != 0) {
        return proc_lseek(pf, offset, whence);
    }
    int32_t new_pos = 0;
    int32_t file_size = (int32_t)pf->fd_inode->i_size;
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = (int32_t)pf->fd_pos + offset;
        break;
    case SEEK_END:
        new_pos = file_size + offset;
        break;
    default:
        return -LSEEK_EINVAL;
    }
    if (new_pos < 0) {
        return -LSEEK_EINVAL;
    }
    pf->fd_pos = (uint32_t)new_pos;
    return (int32_t)pf->fd_pos;
}

static int fs_dir_is_empty(struct FS_INODE *dino) {
    uint32_t pos = 0;
    struct FS_DIRENT de;
    while (fs_dir_next(dino, &pos, &de) == 0) {
        if (strcmp(de.filename, ".") != 0 && strcmp(de.filename, "..") != 0) {
            return 0;
        }
    }
    return 1;
}

static int remove_entry_common(const char *pathname, int want_dir,
                               int check_empty) {
    char parent[MAX_PATH_LEN];
    char base[MAX_PATH_LEN];
    if (split_parent_path(pathname, parent, base, MAX_PATH_LEN)) {
        return -1;
    }
    uint32_t pino = get_parent_inode(parent);
    if (pino == 0) {
        return -1;
    }
    uint32_t ino = 0;
    int ft = 0;
    if (fs_lookup_ftype(pathname, &ino, &ft, 0)) {
        current->errno = 2;
        return -1;
    }
    if ((ft == FT_DIRECTORY) != want_dir) {
        current->errno = 2;
        return -1;
    }
    struct FS_INODE par;
    struct FS_INODE obj;
    if (fs_read_inode(pino, &par) || fs_read_inode(ino, &obj)) {
        current->errno = 5;
        return -1;
    }
    if (fs_check_perm(&par, 2u)) {
        current->errno = 13;
        return -1;
    }
    if (check_empty && !fs_dir_is_empty(&obj)) {
        return -1;
    }
    if (fs_remove_entry(&par, base)) {
        return -1;
    }
    fs_truncate_inode(&obj);
    fs_write_inode(obj.i_no, &obj);
    fs_free_inode(obj.i_no);
    return 0;
}

int sys_unlink(const char *pathname) {
    return remove_entry_common(pathname, 0, 0);
}

int fs_rename_path(const char *oldpath, const char *newpath) {
    if (oldpath == NULL || newpath == NULL) {
        current->errno = 14;
        return -1;
    }
    char opar[MAX_PATH_LEN];
    char obase[MAX_PATH_LEN];
    char npar[MAX_PATH_LEN];
    char nbase[MAX_PATH_LEN];
    if (split_parent_path(oldpath, opar, obase, MAX_PATH_LEN) ||
        split_parent_path(newpath, npar, nbase, MAX_PATH_LEN)) {
        current->errno = 22;
        return -1;
    }
    uint32_t opino = get_parent_inode(opar);
    uint32_t npino = get_parent_inode(npar);
    if (opino == 0 || npino == 0) {
        current->errno = 2;
        return -1;
    }
    uint32_t oino = 0;
    int oft = 0;
    if (fs_lookup_ftype(oldpath, &oino, &oft, 0)) {
        current->errno = 2;
        return -1;
    }
    uint32_t exino = 0;
    int exft = 0;
    if (fs_lookup_ftype(newpath, &exino, &exft, 0) == 0) {
        if (exft == FT_DIRECTORY) {
            current->errno = 21;
            return -1;
        }
        struct FS_INODE npar_ino;
        struct FS_INODE ex;
        if (fs_read_inode(npino, &npar_ino) || fs_read_inode(exino, &ex)) {
            current->errno = 5;
            return -1;
        }
        if (fs_check_perm(&npar_ino, 2u)) {
            current->errno = 13;
            return -1;
        }
        if (fs_remove_entry(&npar_ino, nbase)) {
            current->errno = 5;
            return -1;
        }
        fs_truncate_inode(&ex);
        fs_write_inode(exino, &ex);
        fs_free_inode(exino);
    }
    if (oft == FT_DIRECTORY && opino != npino) {
        current->errno = 18;
        return -1;
    }
    struct FS_INODE par;
    if (fs_read_inode(opino, &par)) {
        current->errno = 5;
        return -1;
    }
    if (fs_check_perm(&par, 2u)) {
        current->errno = 13;
        return -1;
    }
    uint8_t dt = oft == FT_DIRECTORY ? FS_DT_DIR
                 : oft == FT_SYMLINK ? FS_DT_LNK
                                     : 1u;
    if (fs_add_entry_dt(&par, oino, nbase, dt)) {
        current->errno = 5;
        return -1;
    }
    struct FS_INODE par2;
    if (fs_read_inode(opino, &par2) || fs_remove_entry(&par2, obase)) {
        current->errno = 5;
        return -1;
    }
    return 0;
}

int fs_truncate_path(const char *path, uint32_t length) {
    if (path == NULL) {
        current->errno = 14;
        return -1;
    }
    uint32_t ino = 0;
    int ft = 0;
    if (fs_lookup_ftype(path, &ino, &ft, 1)) {
        current->errno = 2;
        return -1;
    }
    if (ft != FT_REGULAR) {
        current->errno = 22;
        return -1;
    }
    struct FS_INODE obj;
    if (fs_read_inode(ino, &obj)) {
        current->errno = 5;
        return -1;
    }
    if (fs_check_perm(&obj, 2u)) {
        current->errno = 13;
        return -1;
    }
    uint32_t old = obj.i_size;
    if (length < old) {
        uint8_t zeros[256];
        memset(zeros, 0, sizeof(zeros));
        uint32_t off = length;
        while (off < old) {
            uint32_t chunk = old - off;
            if (chunk > sizeof(zeros)) {
                chunk = sizeof(zeros);
            }
            if (fs_write_to_inode(&obj, off, zeros, chunk) < 0) {
                current->errno = 5;
                return -1;
            }
            off += chunk;
        }
        obj.i_size = length;
        return fs_write_inode(ino, &obj) ? -1 : 0;
    }
    if (length > old) {
        uint8_t zeros[256];
        memset(zeros, 0, sizeof(zeros));
        uint32_t off = old;
        while (off < length) {
            uint32_t chunk = length - off;
            if (chunk > sizeof(zeros)) {
                chunk = sizeof(zeros);
            }
            if (fs_write_to_inode(&obj, off, zeros, chunk) < 0) {
                current->errno = 28;
                return -1;
            }
            off += chunk;
        }
        return fs_write_inode(ino, &obj) ? -1 : 0;
    }
    return 0;
}

int32_t sys_mkdir(const char *pathname) {
    if (pathname == NULL) {
        return -1;
    }
    uint32_t perms = 0o777u & ~current->umask;
    int r = fs_create_common(pathname, 0x4000u | perms, 1);
    return r > 0 ? 0 : -1;
}

#define OPEN_DIR_MAX 16
static struct FS_DIR *open_dir_table[OPEN_DIR_MAX];

static int open_dir_registered(struct FS_DIR *dir) {
    for (int i = 0; i < OPEN_DIR_MAX; i++) {
        if (open_dir_table[i] == dir) {
            return 1;
        }
    }
    return 0;
}

struct FS_DIR *sys_opendir(const char *name) {
    uint32_t ino = 0;
    int is_dir = 0;
    if (!strcmp(name, "/") || !strcmp(name, "/.") || !strcmp(name, "/..")) {
        ino = 2;
        is_dir = 1;
    } else if (fs_lookup(name, &ino, &is_dir)) {
        return NULL;
    }
    if (!is_dir) {
        return NULL;
    }
    struct FS_INODE obj;
    if (fs_read_inode(ino, &obj)) {
        return NULL;
    }
    if (fs_check_perm(&obj, 4u)) {
        current->errno = 13;
        return NULL;
    }
    struct FS_DIR *dir = dir_open(cur_part, ino);
    if (dir == NULL) {
        return NULL;
    }
    int registered = 0;
    for (int i = 0; i < OPEN_DIR_MAX; i++) {
        if (open_dir_table[i] == NULL) {
            open_dir_table[i] = dir;
            registered = 1;
            break;
        }
    }
    if (!registered) {
        dir_close(dir);
        current->errno = 24;
        return NULL;
    }
    return dir;
}

int32_t sys_closedir(struct FS_DIR *dir) {
    int32_t ret = -1;
    if (dir != NULL && open_dir_registered(dir)) {
        for (int i = 0; i < OPEN_DIR_MAX; i++) {
            if (open_dir_table[i] == dir) {
                open_dir_table[i] = NULL;
                break;
            }
        }
        dir_close(dir);
        ret = 0;
    }
    return ret;
}

struct FS_DIRENT *sys_readdir(struct FS_DIR *dir) {
    if (!open_dir_registered(dir)) {
        return NULL;
    }
    return dir_read(dir);
}

void sys_rewinddir(struct FS_DIR *dir) {
    if (open_dir_registered(dir)) {
        dir_rewind(dir);
    }
}

int32_t sys_rmdir(const char *pathname) {
    if (pathname == NULL) {
        return -1;
    }
    if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") ||
        !strcmp(pathname, "/..") || !strcmp(pathname, ".") ||
        !strcmp(pathname, "..")) {
        return -1;
    }
    return remove_entry_common(pathname, 1, 1);
}

static uint32_t get_parent_dir_inode_nr(uint32_t child_inode_nr) {
    struct FS_INODE *ino = inode_open(cur_part, child_inode_nr);
    if (ino == NULL) {
        return (uint32_t)-1;
    }
    uint32_t pos = 0;
    struct FS_DIRENT de;
    uint32_t parent = 0;
    while (fs_dir_next(ino, &pos, &de) == 0) {
        if (strcmp(de.filename, "..") == 0) {
            parent = de.i_no;
            break;
        }
    }
    inode_close(ino);
    return parent;
}

static int get_child_dir_name(uint32_t p_inode_nr, uint32_t c_inode_nr,
                              char *path) {
    struct FS_INODE *p = inode_open(cur_part, p_inode_nr);
    if (p == NULL) {
        return -1;
    }
    uint32_t pos = 0;
    struct FS_DIRENT de;
    int ret = -1;
    while (fs_dir_next(p, &pos, &de) == 0) {
        if (de.i_no == c_inode_nr && strcmp(de.filename, ".") != 0 &&
            strcmp(de.filename, "..") != 0) {
            strcat(path, "/");
            strcat(path, de.filename);
            ret = 0;
            break;
        }
    }
    inode_close(p);
    return ret;
}

char *sys_getcwd(char *buf, uint32_t size) {
    if (buf == NULL || size == 0) {
        return NULL;
    }
    return fs_cwd_abs_prefix(buf, size) == 0 ? buf : NULL;
}

int fs_inode_abs_path(uint32_t ino, char *buf, uint32_t size) {
    if (size < 2) {
        return -1;
    }
    if (ino == 0 || ino == ROOT_DIR_INODE_NR) {
        buf[0] = '/';
        buf[1] = 0;
        return 0;
    }
    char reverse[MAX_PATH_LEN] = {0};
    uint32_t child = ino;
    while (child != ROOT_DIR_INODE_NR && child != 0) {
        uint32_t parent = get_parent_dir_inode_nr(child);
        if (parent == 0 || parent == (uint32_t)-1 || parent == child) {
            return -1;
        }
        if (get_child_dir_name(parent, child, reverse) == -1) {
            return -1;
        }
        child = parent;
    }
    buf[0] = 0;
    char *last_slash;
    while ((last_slash = strrchr(reverse, '/'))) {
        uint32_t len = strlen(buf);
        uint32_t seg_len = strlen(last_slash);
        if (len + seg_len + 1 > size) {
            return -1;
        }
        strcpy(buf + len, last_slash);
        *last_slash = 0;
    }
    if (buf[0] == 0) {
        buf[0] = '/';
        buf[1] = 0;
    }
    return 0;
}

int fs_cwd_abs_prefix(char *buf, uint32_t size) {
    uint32_t cwd = (current != NULL) ? current->cwd_inode_nr : ROOT_DIR_INODE_NR;
    return fs_inode_abs_path(cwd, buf, size);
}

int32_t sys_chdir(const char *path) {
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(path, &ino, &is_dir) || !is_dir) {
        return -1;
    }
    struct FS_INODE obj;
    if (fs_read_inode(ino, &obj)) {
        return -1;
    }
    if (fs_check_perm(&obj, 1u)) {
        current->errno = 13;
        return -1;
    }
    current->cwd_inode_nr = ino;
    return 0;
}

int32_t sys_stat(const char *path, struct FS_STAT *buf) {
    if (path == NULL) {
        return -1;
    }
    if (proc_match(path)) {
        return proc_stat(path, buf);
    }
    if (!strcmp(path, ".") || !strcmp(path, "/.") || !strcmp(path, "/..")) {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_ino = 2;
        buf->st_size = 0;
        return 0;
    }
    uint32_t ino = 0;
    int is_dir = 0;
    if (fs_lookup(path, &ino, &is_dir)) {
        return -1;
    }
    struct FS_INODE *obj = inode_open(cur_part, ino);
    if (obj == NULL) {
        return -1;
    }
    buf->st_size = obj->i_size;
    buf->st_filetype = fs_is_chardev(obj) ? FT_CHARDEVICE
                       : is_dir           ? FT_DIRECTORY
                                          : FT_REGULAR;
    inode_close(obj);
    buf->st_ino = ino;
    return 0;
}
