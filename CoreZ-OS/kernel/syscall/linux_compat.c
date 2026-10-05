#include "kernel/syscall/linux_compat.h"
#include "drivers/char/console/io.h"
#include "arch/interrupt/interrupt.h"
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














#define LC_ARGS                                                              \
    struct ARCH_REGS *r, uint64_t a, uint64_t b, uint64_t c, uint64_t d,      \
        uint64_t e, uint64_t f




























































void lc_seterrno(struct TASK *cur, int32_t val) {
    cur->errno = val;
    if (cur->tls_selector == SELECTOR_TLS && cur->tls_base != 0) {
        *(volatile int32_t *)cur->tls_base = val;
    }
}

typedef int64_t (*LcFn)(struct ARCH_REGS *r, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f);






__attribute__((noreturn)) int64_t lc_exit_group(struct ARCH_REGS *r,
                                                       uint64_t a, uint64_t b,
                                                       uint64_t c, uint64_t d,
                                                       uint64_t e, uint64_t f) {
    (void)r;
    sys_exit((int32_t)a);
    for (;;) {
    }
}


































































































#define LC_TABLE_SIZE 327

static const LcFn LC_TABLE[LC_TABLE_SIZE] = {
    [SYS_LINUX_read] = lc_read,
    [SYS_LINUX_pread64] = lc_pread64,
    [SYS_LINUX_write] = lc_write,
    [SYS_LINUX_open] = lc_open,
    [SYS_LINUX_close] = lc_close,
    [SYS_LINUX_exit] = lc_exit,
    [SYS_LINUX_exit_group] = lc_exit_group,
    [SYS_LINUX_brk] = lc_brk,
    [SYS_LINUX_mmap] = lc_mmap,
    [SYS_LINUX_munmap] = lc_munmap,
    [SYS_LINUX_set_thread_area] = lc_set_thread_area,
    [SYS_LINUX_set_tid_address] = lc_set_tid_address,
    [SYS_LINUX_gettid] = lc_gettid,
    [SYS_LINUX_writev] = lc_writev,
    [SYS_LINUX_getpid] = lc_getpid,
    [SYS_LINUX_getppid] = lc_getppid,
    [SYS_LINUX_fstat] = lc_fstat,
    [SYS_LINUX_stat] = lc_stat,
    [SYS_LINUX_lstat] = lc_lstat,
    [SYS_LINUX_lseek] = lc_lseek,
    [SYS_LINUX_fcntl] = lc_fcntl,
    [SYS_LINUX_flock] = lc_flock,
    [SYS_LINUX_readlink] = lc_readlink,
    [SYS_LINUX_chdir] = lc_chdir,
    [SYS_LINUX_getcwd] = lc_getcwd,
    [SYS_LINUX_mkdir] = lc_mkdir,
    [SYS_LINUX_rmdir] = lc_rmdir,
    [SYS_LINUX_unlink] = lc_unlink,
    [SYS_LINUX_rename] = lc_rename,
    [SYS_LINUX_chmod] = lc_chmod,
    [SYS_LINUX_fchmod] = lc_fchmod,
    [SYS_LINUX_fchmodat] = lc_fchmodat,
    [SYS_LINUX_chown] = lc_chown,
    [SYS_LINUX_fchown] = lc_fchown,
    [SYS_LINUX_lchown] = lc_lchown,
    [SYS_LINUX_fchownat] = lc_fchownat,
    [SYS_LINUX_getuid] = lc_getuid,
    [SYS_LINUX_getgid] = lc_getgid,
    [SYS_LINUX_geteuid] = lc_geteuid,
    [SYS_LINUX_getegid] = lc_getegid,
    [SYS_LINUX_setuid] = lc_setuid,
    [SYS_LINUX_setgid] = lc_setgid,
    [SYS_LINUX_setreuid] = lc_setreuid,
    [SYS_LINUX_setregid] = lc_setregid,
    [SYS_LINUX_setresuid] = lc_setresuid,
    [SYS_LINUX_setresgid] = lc_setresgid,
    [SYS_LINUX_getresuid] = lc_getresuid,
    [SYS_LINUX_getresgid] = lc_getresgid,
    [SYS_LINUX_getgroups] = lc_getgroups,
    [SYS_LINUX_setgroups] = lc_setgroups,
    [SYS_LINUX_symlink] = lc_symlink,
    [SYS_LINUX_symlinkat] = lc_symlinkat,
    [SYS_LINUX_mknod] = lc_mknod,
    [SYS_LINUX_mknodat] = lc_mknodat,
    [SYS_LINUX_access] = lc_access,
    [SYS_LINUX_kill] = lc_kill,
    [SYS_LINUX_futex] = lc_futex,
    [SYS_LINUX_gettimeofday] = lc_gettimeofday,
    [SYS_LINUX_nanosleep] = lc_nanosleep,
    [SYS_LINUX_clock_nanosleep] = lc_clock_nanosleep,
    [SYS_LINUX_clock_gettime] = lc_clock_gettime,
    [SYS_LINUX_clock_getres] = lc_clock_getres,
    [SYS_LINUX_mprotect] = lc_mprotect,
    [SYS_LINUX_rt_sigaction] = lc_rt_sigaction,
    [SYS_LINUX_rt_sigprocmask] = lc_rt_sigprocmask,
    [SYS_LINUX_getdents64] = lc_getdents64,
    [SYS_LINUX_ioctl] = lc_ioctl,
    [SYS_LINUX_readv] = lc_readv,
    [SYS_LINUX_wait4] = lc_wait4,
    [SYS_LINUX_uname] = lc_uname,
    [SYS_LINUX_sysinfo] = lc_sysinfo,
    [SYS_LINUX_times] = lc_times,
    [SYS_LINUX_ftruncate] = lc_ftruncate,
    [SYS_LINUX_rt_sigreturn] = lc_rt_sigreturn,
    [SYS_LINUX_setpgid] = lc_setpgid,
    [SYS_LINUX_setsid] = lc_setsid,
    [SYS_LINUX_sigaltstack] = lc_sigaltstack,
    [SYS_LINUX_rt_sigsuspend] = lc_sigsuspend,
    [SYS_LINUX_getitimer] = lc_getitimer,
    [SYS_LINUX_setitimer] = lc_setitimer,
    [SYS_LINUX_getrusage] = lc_getrusage,
    [SYS_LINUX_statfs] = lc_statfs,
    [SYS_LINUX_fstatfs] = lc_fstatfs,
    [SYS_LINUX_waitid] = lc_waitid,
    [SYS_LINUX_socket] = lc_socket,
    [SYS_LINUX_connect] = lc_connect,
    [SYS_LINUX_accept] = lc_accept,
    [SYS_LINUX_sendto] = lc_sendto,
    [SYS_LINUX_recvfrom] = lc_recvfrom,
    [SYS_LINUX_sendmsg] = lc_sendmsg,
    [SYS_LINUX_recvmsg] = lc_recvmsg,
    [SYS_LINUX_shutdown] = lc_shutdown,
    [SYS_LINUX_bind] = lc_bind,
    [SYS_LINUX_listen] = lc_listen,
    [SYS_LINUX_getsockname] = lc_getsockname,
    [SYS_LINUX_getpeername] = lc_getpeername,
    [SYS_LINUX_socketpair] = lc_socketpair,
    [SYS_LINUX_setsockopt] = lc_setsockopt,
    [SYS_LINUX_getsockopt] = lc_getsockopt,
    [SYS_LINUX_tkill] = lc_tkill,
    [SYS_LINUX_umask] = lc_umask,
    [SYS_LINUX_tgkill] = lc_tgkill,
    [SYS_LINUX_getsid] = lc_getsid,
    [SYS_LINUX_getpgid] = lc_getpgid,
    [SYS_LINUX_arch_prctl] = lc_arch_prctl,
    [SYS_LINUX_sched_yield] = lc_sched_yield,
    [SYS_LINUX_execve] = lc_execve,
    [SYS_LINUX_fork] = lc_fork,
    [SYS_LINUX_vfork] = lc_fork,
    [SYS_LINUX_clone] = lc_clone,
    [SYS_LINUX_pipe] = lc_pipe,
    [SYS_LINUX_pipe2] = lc_pipe2,
    [SYS_LINUX_dup] = lc_dup,
    [SYS_LINUX_dup2] = lc_dup2,
    [SYS_LINUX_dup3] = lc_dup3,
    [SYS_LINUX_openat] = lc_openat,
    [SYS_LINUX_newfstatat] = lc_newfstatat,
    [SYS_LINUX_unlinkat] = lc_unlinkat,
    [SYS_LINUX_utimensat] = lc_utimensat,
    [SYS_LINUX_mkdirat] = lc_mkdirat,
    [SYS_LINUX_renameat] = lc_renameat,
    [SYS_LINUX_renameat2] = lc_renameat2,
    [SYS_LINUX_readlinkat] = lc_readlinkat,
    [SYS_LINUX_faccessat] = lc_faccessat,
    [SYS_LINUX_getrandom] = lc_getrandom,
    [SYS_LINUX_copy_file_range] = lc_copy_file_range,
    [SYS_LINUX_poll] = lc_poll,
    [SYS_LINUX_ppoll] = lc_ppoll,
    [SYS_LINUX_select] = lc_select,
    [SYS_LINUX_pselect6] = lc_pselect6,
    [SYS_LINUX_epoll_create] = lc_epoll_create1,
    [SYS_LINUX_epoll_create1] = lc_epoll_create1,
    [SYS_LINUX_epoll_ctl] = lc_epoll_ctl,
    [SYS_LINUX_epoll_wait] = lc_epoll_wait,
    [SYS_LINUX_epoll_pwait] = lc_epoll_pwait,
    [SYS_LINUX_eventfd] = lc_eventfd,
    [SYS_LINUX_eventfd2] = lc_eventfd2,
    [SYS_LINUX_timerfd_create] = lc_timerfd_create,
    [SYS_LINUX_timerfd_settime] = lc_timerfd_settime,
    [SYS_LINUX_timerfd_gettime] = lc_timerfd_gettime,
    [SYS_LINUX_getrlimit] = lc_getrlimit,
    [SYS_LINUX_setrlimit] = lc_setrlimit,
    [SYS_LINUX_prlimit64] = lc_prlimit64,
    [SYS_LINUX_madvise] = lc_madvise,
    [SYS_LINUX_fsync] = lc_fsync,
    [SYS_LINUX_fdatasync] = lc_fsync,
    [SYS_LINUX_truncate] = lc_truncate,
};

int64_t linux_compat_handler(struct ARCH_REGS *r) {
    struct TASK *cur = current;
    uint32_t nr = r->eax;
    int64_t ret = -LINUX_ENOSYS;
    int32_t saved_errno = cur->errno;

    cur->errno = 0;
    if (nr >= COMPAT_SYSCALL_BASE) {
        static const LcFn LC0_TABLE[] = {
            [0] = lc_getpid,  [1] = lc_write, [2] = lc_read,
            [3] = lc_exit,    [4] = lc_brk,   [5] = lc0_open,
            [6] = lc_close,   [7] = lc_mmap,  [8] = lc_set_thread_area,
            [9] = lc0_writev,
        };
        uint32_t idx = nr - COMPAT_SYSCALL_BASE;
        if (idx < sizeof(LC0_TABLE) / sizeof(LC0_TABLE[0]) && LC0_TABLE[idx])
            ret = LC0_TABLE[idx](r, r->rbx, r->rcx, r->rdx, r->rsi, r->rdi,
                                 r->rbp);
    } else if (nr < LC_TABLE_SIZE && LC_TABLE[nr]) {
        ret = LC_TABLE[nr](r, r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9);
    }

    if (ret == -1 && cur->errno > 0)
        ret = -(int64_t)cur->errno;
    lc_seterrno(cur, ret < 0 ? (int32_t)-ret : saved_errno);
    return ret;
}
