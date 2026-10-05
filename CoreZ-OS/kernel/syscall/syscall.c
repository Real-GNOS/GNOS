#include "kernel/syscall/syscall.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/rtc.h"
#include "drivers/char/tty.h"
#include "drivers/net/net.h"
#include "drivers/net/socket.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/dir.h"
#include "kernel/gui/gui.h"
#include "kernel/init/acpi/acpi.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/shell/pipe.h"
#include "kernel/syscall/file_syscall.h"
#include "kernel/syscall/futex.h"
#include "kernel/syscall/linux_compat.h"
#include "kernel/syscall/mmap.h"
#include "kernel/syscall/win32.h"
#include "kernel/userprog/clone.h"
#include "kernel/userprog/exec.h"
#include "kernel/userprog/fork.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/str/str.h"
#include "libc/user/syscall.h"
static uint32_t sys_getpid(void) {
    return current->pid;
}

static int32_t sys_clock_gettime(int32_t clk_id, struct SYS_TIMESPEC *tp) {
    if (tp == NULL) {
        return -1;
    }
    memset(tp, 0, sizeof(*tp));
    if (clk_id == 0) {
        tp->tv_sec = (int32_t)rtc_unix_time();
        tp->tv_nsec = 0;
        return 0;
    }
    tp->tv_sec = (int32_t)(tick / PIT_HZ);
    tp->tv_nsec = (int32_t)((tick % PIT_HZ) * (1000u * 1000u * 1000u / PIT_HZ));
    return 0;
}

static int32_t sys_gettimeofday(struct SYS_TIMEVAL *tv, void *tz) {
    if (tv == NULL) {
        return -1;
    }
    (void)tz;
    memset(tv, 0, sizeof(*tv));
    tv->tv_sec = (int32_t)rtc_unix_time();
    tv->tv_usec = 0;
    return 0;
}

static int32_t sys_nanosleep(const struct SYS_TIMESPEC *req, struct SYS_TIMESPEC *rem) {
    if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0) {
        return -1;
    }
    uint32_t sec = (uint32_t)req->tv_sec;
    uint32_t ms;
    if (sec > 0x1fffff) {
        ms = 0x7fffffffU;
    } else {
        ms = sec * 1000u;
    }
    ms += (uint32_t)req->tv_nsec / 1000000u;
    if (ms > 0x7fffffffU) {
        ms = 0x7fffffffU;
    }
    if (mtime_sleep_interruptible(ms) == -EINTR) {
        if (rem != NULL) {
            uint64_t ns = (uint64_t)current->sleep_left *
                          (uint64_t)(1000000000u / PIT_HZ);
            rem->tv_sec = (int32_t)(ns / 1000000000ull);
            rem->tv_nsec = (int32_t)(ns % 1000000000ull);
        }
        return -EINTR;
    }
    if (rem != NULL) {
        memset(rem, 0, sizeof(*rem));
    }
    return 0;
}

static uint32_t sys_getid(void) {
    return 0;
}

static void sys_exit_group(int32_t status) {
    sys_exit(status);
    for (;;) {
    }
}

static uint32_t sys_shutdown(void) {
    kprintf("[shutdown] shutting down system...\n");
    acpi_shutdown();
    return 0;
}

static uint32_t sys_write(int32_t fd, char *str, uint32_t count) {
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC) {
        return (uint32_t)-1;
    }
    if (is_pipe(fd)) {
        return pipe_write(fd, str, count);
    }
    uint32_t gfd = fd_local2global((uint32_t)fd);
    struct FILE *wf = file_get(gfd);
    if (gfd >= 3 && wf != NULL && wf->fd_inode != NULL && !is_pipe((uint32_t)fd)) {
        return write_file(fd, str, count);
    }
    TTY.write(str, count);
    return count;
}

static uint32_t sys_putchar(char c) {
    console_putc(c);
    return (uint32_t)(unsigned char)c;
}

static uint32_t sys_clear(void) {
    io_clear_screen();
    return 0;
}

static int32_t sys_read(int32_t fd, void *buf, uint32_t count) {
    if (fd == 1 || fd == 2)
        return -1;
    if (is_pipe(fd)) {
        return (int32_t)pipe_read(fd, buf, count);
    }
    if (fd == 0) {
        return TTY.read((char *)buf, count);
    }
    if (fd < 0 || fd >= (int32_t)MAX_FILES_OPEN_PER_PROC)
        return -1;
    if (count == 0)
        return 0;
    int32_t r = (int32_t)read_file(fd, buf, count);
    return r;
}

static const char *task_status_str(enum TASK_STATUS s) {
    static const char *names[] = {"RUNNING", "READY",  "BLOCKED",
                                  "WAITING", "HANGING", "DIED"};
    return (s >= TASK_RUNNING && s <= TASK_DIED)
               ? names[__builtin_ctz(s)]
               : "?";
}

static int ps_action(struct TASK *t, void *arg) {
    (void)arg;
    char buf[80];
    const char *parent = (t->parent_pid == -1) ? "(none)" : "?";
    if (t->parent_pid >= 0) {
        u32_to_dec((uint32_t)t->parent_pid, buf);
        parent = buf;
    }
    kprintf("PID=%u PPID=%s STAT=%s TICKS=%u NAME=%s\n", t->pid, parent,
            task_status_str(t->status), t->elapsed_ticks, t->name);
    return 0;
}

static uint32_t sys_ps(void) {
    kprintf("=== ps ===\n");
    thread_traverse_all(ps_action, NULL);
    return 0;
}

uint32_t sys_brk(uint32_t addr) {
    struct TASK *cur = current;
    uint32_t base = (cur->brk_base != 0) ? cur->brk_base : USER_HEAP_BASE;
    if (cur->user_brk == 0) {
        cur->user_brk = base;
    }

    uint32_t limit = (base < USER_LOW_CEILING) ? USER_LOW_CEILING
                                               : USER_HEAP_LIMIT;
    uint32_t cur_brk = cur->user_brk;
    if (addr == 0) {
        return cur_brk;
    }
    uint32_t new_brk = addr;
    if (new_brk < base)
        new_brk = base;
    if (new_brk > limit)
        new_brk = limit;
    uint32_t old_page = (cur_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t new_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (new_page > old_page) {
        for (uint32_t page = old_page; page < new_page; page += PAGE_SIZE) {
            if (page_is_mapped(page)) {
                kprintf("[brk] collision at 0x%x, keep 0x%x pid=%d base=%x\n",
                        page, cur_brk, (int)cur->pid, base);
                return cur_brk;
            }
            if (get_a_page(page) == 0) {
                kprintf("[brk] OOM, keep 0x%x\n", cur_brk);
                return cur_brk;
            }
        }
    } else if (new_page < old_page) {
        for (uint32_t page = new_page; page < old_page; page += PAGE_SIZE) {
            free_user_page(page);
        }
    }
    cur->user_brk = new_brk;
    return new_brk;
}

static uint32_t sys_set_thread_area(struct ARCH_REGS *r, uint32_t base) {
    if (base == 0 || !user_range_writable(base, sizeof(int32_t)))
        return (uint32_t)-1;
    current->tls_base = base;
    current->tls_selector = SELECTOR_TLS;
    current->tls_msr = 0;
    tls_desc_set_base(base);
    current->errno = 0;
    *(volatile int32_t *)base = 0;
    return 0;
}

static int kern_call(struct ARCH_REGS *r) {
    return (r->cs & 3) == 0;
}

static int ok_read(struct ARCH_REGS *r, uint32_t p, uint32_t n) {
    return kern_call(r) || access_ok((const void *)p, (size_t)n, 0);
}

static int ok_write(struct ARCH_REGS *r, uint32_t p, uint32_t n) {
    return kern_call(r) || access_ok((const void *)p, (size_t)n, 1);
}

static const char *path_arg_reg(struct ARCH_REGS *r, uint32_t reg, char *kbuf,
                                uint32_t cap) {
    if (kern_call(r)) {
        return (const char *)reg;
    }
    if (copy_str_from_user(kbuf, (const char *)(uintptr_t)reg, cap) != 0) {
        return NULL;
    }
    return kbuf;
}

static const char *path_arg(struct ARCH_REGS *r, char *kbuf, uint32_t cap) {
    return path_arg_reg(r, (uint32_t)r->ebx, kbuf, cap);
}

static int64_t nsys_getpid(struct ARCH_REGS *r) {
    (void)r;
    return sys_getpid();
}

static int64_t nsys_write(struct ARCH_REGS *r) {
    if (!ok_read(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return sys_write((int32_t)r->ebx, (char *)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_putchar(struct ARCH_REGS *r) {
    return sys_putchar((char)r->ebx);
}

static int64_t nsys_clear(struct ARCH_REGS *r) {
    (void)r;
    return sys_clear();
}

static int64_t nsys_read(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_read((int32_t)r->ebx, (void *)r->ecx,
                              (uint32_t)r->edx);
}

static int64_t nsys_fork(struct ARCH_REGS *r) {
    return (uint32_t)sys_fork(r);
}

static int64_t nsys_getcwd(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ebx, r->ecx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_getcwd((char *)r->ebx, (uint32_t)r->ecx);
}

static int64_t nsys_chdir(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_chdir(p);
}

static int64_t nsys_mkdir(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_mkdir(p);
}

static int64_t nsys_rmdir(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_rmdir(p);
}

static int64_t nsys_open(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)open_file(p, (uint8_t)r->ecx);
}

static int64_t nsys_close(struct ARCH_REGS *r) {
    return (uint32_t)close_file((int)r->ebx);
}

static int64_t nsys_lseek(struct ARCH_REGS *r) {
    return (uint32_t)sys_lseek((int32_t)r->ebx, (int32_t)r->ecx,
                               (uint8_t)r->edx);
}

static int64_t nsys_unlink(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_unlink(p);
}

static int64_t nsys_opendir(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)(uintptr_t)sys_opendir(p);
}

static int64_t nsys_closedir(struct ARCH_REGS *r) {
    return (uint32_t)sys_closedir((struct FS_DIR *)r->ebx);
}

static int64_t nsys_readdir(struct ARCH_REGS *r) {
    struct FS_DIRENT *dir_e =
        sys_readdir((struct FS_DIR *)(uintptr_t)r->ebx);
    if (dir_e == NULL) {
        return 0;
    }
    if (!ok_write(r, r->ecx, sizeof(struct FS_DIRENT))) {
        return 0;
    }
    if (copy_to_user((void *)r->ecx, dir_e, sizeof(struct FS_DIRENT)) != 0) {
        return 0;
    }
    return r->ecx;
}

static int64_t nsys_rewinddir(struct ARCH_REGS *r) {
    sys_rewinddir((struct FS_DIR *)r->ebx);
    return 0;
}

static int64_t nsys_stat(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL || !ok_write(r, r->ecx, sizeof(struct FS_STAT))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_stat(p, (struct FS_STAT *)r->ecx);
}

static int64_t nsys_ps(struct ARCH_REGS *r) {
    (void)r;
    sys_ps();
    return 0;
}

static int64_t nsys_execv(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_execv(p, (const char **)r->ecx, r);
}

static int64_t nsys_exit(struct ARCH_REGS *r) {
    sys_exit((int32_t)r->ebx);
    return 0;
}

static int64_t nsys_wait(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ebx, sizeof(int32_t))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_wait((int32_t *)r->ebx);
}

static int64_t nsys_pipe(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ebx, 2 * sizeof(int32_t))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_pipe((int32_t *)r->ebx);
}

static int64_t nsys_fd_redirect(struct ARCH_REGS *r) {
    sys_fd_redirect((uint32_t)r->ebx, (uint32_t)r->ecx);
    return 0;
}

static int64_t nsys_gui(struct ARCH_REGS *r) {
    (void)r;
    return (uint32_t)gui_session_run();
}

static int64_t nsys_brk(struct ARCH_REGS *r) {
    return (uint32_t)sys_brk((uint32_t)r->ebx);
}

static int64_t nsys_sigaction(struct ARCH_REGS *r) {
    if ((r->ecx && !ok_read(r, r->ecx, sizeof(struct SYS_SIGACTION))) ||
        (r->edx && !ok_write(r, r->edx, sizeof(struct SYS_SIGACTION)))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_sigaction((int)r->ebx,
                                   (const struct SYS_SIGACTION *)r->ecx,
                                   (struct SYS_SIGACTION *)r->edx);
}

static int64_t nsys_kill(struct ARCH_REGS *r) {
    return (uint32_t)sys_kill((int)r->ebx, (int)r->ecx);
}

static int64_t nsys_sigreturn(struct ARCH_REGS *r) {
    return sys_sigreturn(r);
}

static int64_t nsys_sigprocmask(struct ARCH_REGS *r) {
    if ((r->ecx && !ok_read(r, r->ecx, sizeof(sigset_t))) ||
        (r->edx && !ok_write(r, r->edx, sizeof(sigset_t)))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_sigprocmask((int)r->ebx, (const sigset_t *)r->ecx,
                                     (sigset_t *)r->edx);
}

static int64_t nsys_set_thread_area(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ebx, sizeof(int32_t))) {
        return (uint32_t)-1;
    }
    return sys_set_thread_area(r, (uint32_t)r->ebx);
}

static int64_t nsys_mmap(struct ARCH_REGS *r) {
    if (!ok_read(r, r->ebx, sizeof(struct SYS_MMAP_ARGS))) {
        return (uint32_t)-1;
    }
    uint32_t ret = sys_mmap((const struct SYS_MMAP_ARGS *)r->ebx);
    return ret > (uint32_t)-4096 ? (uint32_t)-1 : ret;
}

static int64_t nsys_munmap(struct ARCH_REGS *r) {
    return (uint32_t)sys_munmap((uint32_t)r->ebx, (uint32_t)r->ecx);
}

static int64_t nsys_mmap2(struct ARCH_REGS *r) {
    uint32_t ret = sys_mmap2((uint32_t)r->ebx, (uint32_t)r->ecx,
                             (uint32_t)r->edx, (uint32_t)r->esi,
                             (uint32_t)r->edi, (uint32_t)r->r10);
    return ret > (uint32_t)-4096 ? (uint32_t)-1 : ret;
}

static int64_t nsys_mprotect(struct ARCH_REGS *r) {
    return (uint32_t)sys_mprotect((uint32_t)r->ebx, (uint32_t)r->ecx,
                                  (uint32_t)r->edx);
}

static int64_t nsys_futex(struct ARCH_REGS *r) {
    if (!ok_read(r, r->ebx, 4)) {
        return (uint32_t)-1;
    }
    int32_t rc = sys_futex((uint32_t)r->ebx, (uint32_t)r->ecx,
                           (uint32_t)r->edx, (uint32_t)r->esi, 0, 0);
    if (rc == -EINVAL || rc == -ENOSYS) {
        return (uint32_t)-1;
    }
    return (uint32_t)rc;
}

static int64_t nsys_clone(struct ARCH_REGS *r) {
    return (uint32_t)sys_clone(r);
}

static int64_t nsys_fstat(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, sizeof(struct FS_STAT))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_fstat((int32_t)r->ebx, (void *)r->ecx);
}

static int64_t nsys_dup(struct ARCH_REGS *r) {
    return (uint32_t)sys_dup((int32_t)r->ebx);
}

static int64_t nsys_dup2(struct ARCH_REGS *r) {
    return (uint32_t)sys_dup2((int32_t)r->ebx, (int32_t)r->ecx);
}

static int64_t nsys_fcntl(struct ARCH_REGS *r) {
    return (uint32_t)sys_fcntl((int32_t)r->ebx, (int32_t)r->ecx,
                               (uint32_t)r->edx);
}

static int64_t nsys_getdents(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_getdents((int32_t)r->ebx, (void *)r->ecx,
                                  (uint32_t)r->edx);
}

static int64_t nsys_readlink(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL || !ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_readlink(p, (char *)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_access(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_access(p, (int32_t)r->ecx);
}

static int64_t nsys_rename(struct ARCH_REGS *r) {
    char kp_old[MAX_PATH_LEN];
    char kp_new[MAX_PATH_LEN];
    const char *po = path_arg(r, kp_old, MAX_PATH_LEN);
    const char *pn = path_arg_reg(r, r->ecx, kp_new, MAX_PATH_LEN);
    if (po == NULL || pn == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_rename(po, pn);
}

static int64_t nsys_truncate(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_truncate(p, (int32_t)r->ecx);
}

static int64_t nsys_chmod(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_chmod(p, (uint32_t)r->ecx);
}

static int64_t nsys_symlink(struct ARCH_REGS *r) {
    char kp_target[MAX_PATH_LEN];
    char kp_link[MAX_PATH_LEN];
    const char *pt = path_arg(r, kp_target, MAX_PATH_LEN);
    const char *pl = path_arg_reg(r, r->ecx, kp_link, MAX_PATH_LEN);
    if (pt == NULL || pl == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_symlink(pt, pl);
}

static int64_t nsys_mknod(struct ARCH_REGS *r) {
    char kp[MAX_PATH_LEN];
    const char *p = path_arg(r, kp, MAX_PATH_LEN);
    if (p == NULL) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_mknod(p, (uint32_t)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_setfgpid(struct ARCH_REGS *r) {
    extern uint32_t foreground_pid;
    foreground_pid = (uint32_t)r->ebx;
    return 0;
}

__attribute__((noinline)) static void smash_frame(void) {
    char buf[8];
    kprintf("[smash] overflowing kernel stack frame\n");
    for (int32_t i = 0; i < 128; i++) {
        buf[i] = 0x41;
    }
}

static int64_t nsys_smash(struct ARCH_REGS *r) {
    (void)r;
    smash_frame();
    kprintf("[smash] returned, canary failed to detect\n");
    return 0;
}

static int64_t nsys_clock_gettime(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, sizeof(struct SYS_TIMESPEC))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_clock_gettime((int32_t)r->ebx,
                                       (struct SYS_TIMESPEC *)r->ecx);
}

static int64_t nsys_gettimeofday(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ebx, sizeof(struct SYS_TIMEVAL))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_gettimeofday((struct SYS_TIMEVAL *)r->ebx,
                                      (void *)r->ecx);
}

static int64_t nsys_nanosleep(struct ARCH_REGS *r) {
    if (!ok_read(r, r->ebx, sizeof(struct SYS_TIMESPEC))) {
        return (uint32_t)-1;
    }
    return (uint32_t)sys_nanosleep((const struct SYS_TIMESPEC *)r->ebx,
                                   (struct SYS_TIMESPEC *)r->ecx);
}

static int64_t nsys_getid(struct ARCH_REGS *r) {
    (void)r;
    return sys_getid();
}

static int64_t nsys_exit_group(struct ARCH_REGS *r) {
    sys_exit_group((int32_t)r->ebx);
    return 0;
}

static int64_t nsys_icmp_send(struct ARCH_REGS *r) {
    return (uint32_t)nt_icmp_send((uint32_t)r->ebx, (uint16_t)r->ecx,
                                  (uint16_t)r->edx);
}

static int64_t nsys_icmp_recv(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ebx, sizeof(struct NET_PING_REPLY))) {
        return (uint32_t)-1;
    }
    return (uint32_t)nt_icmp_recv((struct NET_PING_REPLY *)r->ebx,
                                  (int)r->ecx);
}

static int64_t nsys_shutdown(struct ARCH_REGS *r) {
    (void)r;
    return sys_shutdown();
}

static int64_t nsys_socket(struct ARCH_REGS *r) {
    return (uint32_t)net_socket((int)r->ebx, (int)r->ecx, (int)r->edx);
}

static int64_t nsys_bind(struct ARCH_REGS *r) {
    return (uint32_t)net_bind((int)r->ebx, (uint32_t)r->ecx,
                              (uint16_t)r->edx);
}

static int64_t nsys_listen(struct ARCH_REGS *r) {
    return (uint32_t)net_listen((int)r->ebx, (int)r->ecx);
}

static int64_t nsys_connect(struct ARCH_REGS *r) {
    return (uint32_t)net_connect((int)r->ebx, (uint32_t)r->ecx,
                                 (uint16_t)r->edx);
}

static int64_t nsys_send(struct ARCH_REGS *r) {
    if (!ok_read(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_send((int)r->ebx, (const void *)r->ecx,
                              (uint32_t)r->edx);
}

static int64_t nsys_recv(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_recv((int)r->ebx, (void *)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_sendto(struct ARCH_REGS *r) {
    if (!ok_read(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_sendto((int)r->ebx, (const void *)r->ecx,
                                (uint32_t)r->edx, (uint32_t)r->esi,
                                (uint16_t)r->edi);
}

static int64_t nsys_recvfrom(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, r->edx)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_recvfrom((int)r->ebx, (void *)r->ecx,
                                  (uint32_t)r->edx, (uint32_t *)r->esi,
                                  (uint16_t *)r->edi);
}

static int64_t nsys_accept(struct ARCH_REGS *r) {
    return (uint32_t)net_accept((int)r->ebx);
}

static int64_t nsys_close_socket(struct ARCH_REGS *r) {
    return (uint32_t)net_close((int)r->ebx);
}

static int64_t nsys_sock_shutdown(struct ARCH_REGS *r) {
    return (uint32_t)net_shutdown((int)r->ebx, (int)r->ecx);
}

static int64_t nsys_getsockname(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, 4) || !ok_write(r, r->edx, 2)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_getsockname((int)r->ebx, (uint32_t *)r->ecx,
                                     (uint16_t *)r->edx);
}

static int64_t nsys_getpeername(struct ARCH_REGS *r) {
    if (!ok_write(r, r->ecx, 4) || !ok_write(r, r->edx, 2)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_getpeername((int)r->ebx, (uint32_t *)r->ecx,
                                     (uint16_t *)r->edx);
}

static int64_t nsys_getsockopt(struct ARCH_REGS *r) {
    uint32_t klen = 0;
    uint32_t kval = 0;
    if (r->edi != 0 &&
        copy_from_user(&klen, (const void *)r->edi, sizeof(klen)) != 0) {
        return (uint32_t)-1;
    }
    if (r->esi != 0 && !ok_write(r, r->esi, sizeof(kval))) {
        return (uint32_t)-1;
    }
    int32_t rc = (int32_t)net_getsockopt((int)r->ebx, (int)r->ecx, (int)r->edx,
                                         r->esi != 0 ? &kval : NULL,
                                         r->edi != 0 ? &klen : NULL);
    if (rc != 0) {
        return (uint32_t)(int64_t)rc;
    }
    if (r->esi != 0 &&
        copy_to_user((void *)r->esi, &kval, sizeof(kval)) != 0) {
        return (uint32_t)-1;
    }
    if (r->edi != 0 &&
        copy_to_user((void *)r->edi, &klen, sizeof(klen)) != 0) {
        return (uint32_t)-1;
    }
    return 0;
}

static int64_t nsys_setsockopt(struct ARCH_REGS *r) {
    if (!ok_read(r, r->esi, r->edi)) {
        return (uint32_t)-1;
    }
    return (uint32_t)net_setsockopt((int)r->ebx, (int)r->ecx, (int)r->edx,
                                    (const void *)r->esi, (uint32_t)r->edi);
}

static int64_t nsys_sock_fcntl(struct ARCH_REGS *r) {
    return (uint32_t)net_fcntl((int)r->ebx, (int)r->ecx, (uint32_t)r->edx);
}

static int64_t nsys_select(struct ARCH_REGS *r) {
    int32_t nfds = (int32_t)r->ebx;
    if (nfds < 0 || (uint32_t)nfds > SEL_FD_SET_FDS) {
        return (uint32_t)-1;
    }
    uint32_t bytes = ((uint32_t)nfds + 31u) / 32u * 4u;
    if (bytes != 0) {
        if ((r->ecx && !ok_write(r, r->ecx, bytes)) ||
            (r->edx && !ok_write(r, r->edx, bytes)) ||
            (r->esi && !ok_write(r, r->esi, bytes))) {
            return (uint32_t)-1;
        }
    }
    return (uint32_t)net_select((int)r->ebx, (uint32_t *)r->ecx,
                                (uint32_t *)r->edx, (uint32_t *)r->esi,
                                (int)r->edi);
}

typedef int64_t (*nsys_fn)(struct ARCH_REGS *r);

static const nsys_fn nsys_table[] = {
    [SYS_GETPID] = nsys_getpid,       [SYS_WRITE] = nsys_write,
    [SYS_READ] = nsys_read,           [SYS_PUTCHAR] = nsys_putchar,
    [SYS_CLEAR] = nsys_clear,         [SYS_FORK] = nsys_fork,
    [SYS_GETCWD] = nsys_getcwd,       [SYS_CHDIR] = nsys_chdir,
    [SYS_MKDIR] = nsys_mkdir,         [SYS_RMDIR] = nsys_rmdir,
    [SYS_OPEN] = nsys_open,           [SYS_CLOSE] = nsys_close,
    [SYS_LSEEK] = nsys_lseek,         [SYS_UNLINK] = nsys_unlink,
    [SYS_OPENDIR] = nsys_opendir,     [SYS_CLOSEDIR] = nsys_closedir,
    [SYS_READDIR] = nsys_readdir,     [SYS_REWINDDIR] = nsys_rewinddir,
    [SYS_STAT] = nsys_stat,           [SYS_PS] = nsys_ps,
    [SYS_EXECV] = nsys_execv,         [SYS_EXIT] = nsys_exit,
    [SYS_WAIT] = nsys_wait,           [SYS_PIPE] = nsys_pipe,
    [SYS_FD_REDIRECT] = nsys_fd_redirect, [SYS_BRK] = nsys_brk,
    [SYS_GUI] = nsys_gui,             [SYS_SIGACTION] = nsys_sigaction,
    [SYS_KILL] = nsys_kill,           [SYS_SIGRETURN] = nsys_sigreturn,
    [SYS_SIGPROCMASK] = nsys_sigprocmask, [SYS_SET_THREAD_AREA] =
        nsys_set_thread_area,
    [SYS_MMAP] = nsys_mmap,           [SYS_MUNMAP] = nsys_munmap,
    [SYS_MPROTECT] = nsys_mprotect,   [SYS_FUTEX] = nsys_futex,
    [SYS_CLONE] = nsys_clone,         [SYS_FSTAT] = nsys_fstat,
    [SYS_DUP] = nsys_dup,             [SYS_DUP2] = nsys_dup2,
    [SYS_FCNTL] = nsys_fcntl,         [SYS_GETDENTS] = nsys_getdents,
    [SYS_READLINK] = nsys_readlink,   [SYS_ACCESS] = nsys_access,
    [SYS_RENAME] = nsys_rename,       [SYS_TRUNCATE] = nsys_truncate,
    [SYS_CHMOD] = nsys_chmod,         [SYS_CLOCK_GETTIME] = nsys_clock_gettime,
    [SYS_GETTIMEOFDAY] = nsys_gettimeofday, [SYS_NANOSLEEP] = nsys_nanosleep,
    [SYS_GETUID] = nsys_getid,        [SYS_GETGID] = nsys_getid,
    [SYS_GETEUID] = nsys_getid,       [SYS_GETEGID] = nsys_getid,
    [SYS_EXIT_GROUP] = nsys_exit_group, [SYS_MMAP2] = nsys_mmap2,
    [SYS_ICMP_SEND] = nsys_icmp_send, [SYS_ICMP_RECV] = nsys_icmp_recv,
    [SYS_SHUTDOWN] = nsys_shutdown,   [SYS_SOCKET] = nsys_socket,
    [SYS_BIND] = nsys_bind,           [SYS_LISTEN] = nsys_listen,
    [SYS_CONNECT] = nsys_connect,     [SYS_SEND] = nsys_send,
    [SYS_RECV] = nsys_recv,           [SYS_SENDTO] = nsys_sendto,
    [SYS_RECVFROM] = nsys_recvfrom,   [SYS_ACCEPT] = nsys_accept,
    [SYS_CLOSE_SOCKET] = nsys_close_socket, [SYS_SOCK_SHUTDOWN] =
        nsys_sock_shutdown,
    [SYS_GETSOCKNAME] = nsys_getsockname, [SYS_GETPEERNAME] =
        nsys_getpeername,
    [SYS_GETSOCKOPT] = nsys_getsockopt, [SYS_SETSOCKOPT] = nsys_setsockopt,
    [SYS_SOCK_FCNTL] = nsys_sock_fcntl, [SYS_SELECT] = nsys_select,
    [SYS_MKNOD] = nsys_mknod,         [SYS_SYMLINK] = nsys_symlink,
    [SYS_SETFGPID] = nsys_setfgpid,   [SYS_SMASH] = nsys_smash,
};

volatile uint32_t sc_total;
uint32_t sc_last_nr[MAX_TASKS];
uint32_t sc_last_a0[MAX_TASKS];
uint32_t sc_last_a1[MAX_TASKS];
uint32_t sc_last_ra[MAX_TASKS];
uint32_t sc_last_stk[MAX_TASKS][16];

#define SC_TRACE_ENABLE 0

static int sc_trace_interest(uint32_t nr) {
#if !SC_TRACE_ENABLE
    (void)nr;
    return 0;
#else
    switch (nr) {
    case 0: case 1: case 3: case 8: case 9: case 10: case 11: case 12: case 16:
    case 32: case 33: case 56: case 57: case 58: case 59: case 60: case 61:
    case 72: case 202: case 231: case 257: case 290: case 293: case 435:
    case 35: case 230: case 270: case 271: case 322:
    case 7: case 23: case 232: case 281:
        return 1;
    }
    return 0;
#endif
}

static void sc_read_str(uint64_t up, char *buf, uint32_t cap) {
    uint32_t i;
    buf[0] = 0;
    if (up < USER_VADDR_START ||
        !user_range_readable((uint32_t)up, cap - 1)) {
        return;
    }
    for (i = 0; i < cap - 1; i++) {
        char ch = ((const char *)(uintptr_t)up)[i];
        if (ch == 0)
            break;
        buf[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
    }
    buf[i] = 0;
}

static void sc_trace_emit(struct ARCH_REGS *r, uint32_t nr) {
    char pbuf[40];
    if (nr == 59 || nr == 257) {
        sc_read_str(r->rdi, pbuf, sizeof(pbuf));
        kprintf("[sc] pid=%d nr=%u path=%s\n", current->pid, nr, pbuf);
        return;
    }
    if (nr == 1) {
        if (r->rdi == 0x800) {
            sc_read_str(r->rsi, pbuf, sizeof(pbuf));
            kprintf("[sc] pid=%d nr=1 fd=EV val=%s\n", current->pid, pbuf);
        } else {
            sc_read_str(r->rsi, pbuf, sizeof(pbuf));
            kprintf("[sc] pid=%d nr=1 fd=%d n=%d txt=%s\n", current->pid,
                    (int)r->rdi, (int)(uint32_t)r->rdx, pbuf);
        }
        return;
    }
    if (nr == 270 || nr == 271) {
        uint32_t nfds = (uint32_t)r->rdi;
        if (nfds > 4)
            nfds = 4;
        kprintf("[sc] pid=%d nr=%u nfds=%d fds=", current->pid, nr,
                (int)(uint32_t)r->rdi);
        for (uint32_t k = 0; k < nfds; k++) {
            uint32_t base = (uint32_t)r->rsi + k * 8;
            int32_t fd = -2;
            if (base >= USER_VADDR_START &&
                user_range_readable(base, 8)) {
                fd = *(const int32_t *)(uintptr_t)base;
            }
            kprintf("%d,", (int)fd);
        }
        kprintf("\n");
        return;
    }
    if (nr == 0) {
        kprintf("[sc] pid=%d nr=0 fd=%d n=%d\n", current->pid, (int)r->rdi,
                (int)(uint32_t)r->rdx);
        return;
    }
    if (nr == 7) {
        uint32_t nfds = (uint32_t)r->rsi;
        if (nfds > 6)
            nfds = 6;
        kprintf("[sc] pid=%d nr=7 nfds=%d fds=", current->pid,
                (int)(uint32_t)r->rsi);
        for (uint32_t k = 0; k < nfds; k++) {
            uint32_t base = (uint32_t)r->rdi + k * 8;
            int32_t pfd = -2;
            int16_t pev = 0;
            if (base >= USER_VADDR_START &&
                user_range_readable(base, 8)) {
                pfd = *(const int32_t *)(uintptr_t)base;
                pev = *(const int16_t *)(uintptr_t)(base + 4);
            }
            kprintf("%d/%x,", pfd, (unsigned)pev);
        }
        kprintf(" tmo=%d\n", (int32_t)(uint32_t)r->rdx);
        return;
    }
    if (nr == 202) {
        kprintf("[sc] pid=%d nr=202 uaddr=%x op=%x val=%x\n", current->pid,
                (uint32_t)r->rdi, (uint32_t)r->rsi, (uint32_t)r->rdx);
        return;
    }
    kprintf("[sc] pid=%d nr=%u a0=%x a1=%x a2=%x b=%x t=%u\n", current->pid,
            nr, (uint32_t)r->rdi, (uint32_t)r->rsi, (uint32_t)r->rdx,
            (uint32_t)current->exe_bias, (unsigned)tick);
}

static int sc_ret_interest(uint32_t nr) {
#if !SC_TRACE_ENABLE
    (void)nr;
    return 0;
#else
    switch (nr) {
    case 0: case 1: case 8: case 9: case 10: case 11: case 12: case 59: case 257:
    case 290: case 293: case 32: case 33: case 72: case 202:
    case 7: case 23: case 232: case 281: case 16:
        return 1;
    }
    return 0;
#endif
}

uint64_t syscall_handler(struct ARCH_REGS *r) {
    uint32_t nr = r->eax;
    uint64_t ret = (uint32_t)-1;
    sc_total++;
    {
        uint32_t sslot = (uint32_t)(current - task_table);
        uint32_t us = (uint32_t)r->user_rsp;
        sc_last_nr[sslot] = nr;
        sc_last_a0[sslot] = (uint32_t)r->rdi;
        sc_last_a1[sslot] = (uint32_t)r->rsi;
        sc_last_ra[sslot] = (uint32_t)(r->cs & 3) ? (uint32_t)r->rip : 0;
        for (uint32_t k = 0; k < 16; k++) {
            uint32_t ua = us + k * 4;
            sc_last_stk[sslot][k] =
                (us >= USER_VADDR_START && ua < 0xc0000000u &&
                 page_is_mapped(ua))
                    ? *(const uint32_t *)(uintptr_t)ua
                    : 0;
        }
    }
    if (r->int_no == 0x81 && sc_trace_interest(nr)) {
        sc_trace_emit(r, nr);
    }
    if (nr >= WIN32_SYSCALL_BASE) {
        ret = (uint64_t)win32_handler(r);
        r->rax = ret;
        check_pending_signals(r);
        return ret;
    }
    if (r->int_no == 0x81 || current->compat || nr >= COMPAT_SYSCALL_BASE) {
        if (r->int_no == 0x80 && nr < COMPAT_SYSCALL_BASE) {
            r->rdi = r->rbx;
            r->rsi = r->rcx;
            r->r10 = r->esi;
            r->r8 = r->edi;
            r->r9 = r->ebp;
        }
        ret = (uint64_t)linux_compat_handler(r);
        r->rax = ret;
        if (r->int_no == 0x81 &&
            (sc_ret_interest(nr) || (sc_trace_interest(nr) &&
                                     ((int64_t)ret < 0)))) {
            kprintf("[sc] pid=%d nr=%u ret=%d\n", current->pid, nr,
                    (int64_t)ret);
        }

        check_pending_signals(r);
        return ret;
    }
    if (nr < sizeof(nsys_table) / sizeof(nsys_table[0]) && nsys_table[nr]) {
        ret = (uint64_t)nsys_table[nr](r);
    }
    r->rax = ret;
    if (r->int_no == 0x81 &&
        (sc_ret_interest(nr) ||
         (sc_trace_interest(nr) && ((int64_t)ret < 0)))) {
        kprintf("[sc] pid=%d nr=%u ret=%d\n", current->pid, nr,
                (int64_t)ret);
    }
    check_pending_signals(r);
    return ret;
}

void syscall_init(void) {
    extern void syscall_entry(void);
    const uint64_t MSR_STAR = 0xC0000081;
    const uint64_t MSR_LSTAR = 0xC0000082;
    const uint64_t MSR_FMASK = 0xC0000084;
    const uint64_t MSR_EFER = 0xC0000080;

    uint64_t star = ((uint64_t)0x33 << 48) | ((uint64_t)0x08 << 32);
    asm_wrmsr(MSR_STAR, star);
    asm_wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    asm_wrmsr(MSR_FMASK, 0x5700);
    uint64_t efer = asm_rdmsr(MSR_EFER);
    asm_wrmsr(MSR_EFER, efer | 1);

    kprintf("[OK] syscall init, 0x80 full table + syscall/sysret entry\n");
}
