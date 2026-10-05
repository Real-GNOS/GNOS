#ifndef LC_INTERNAL_H
#define LC_INTERNAL_H

#include <stdint.h>

#include "kernel/asm/stub.h"
#include "kernel/fs/fs.h"
#include "kernel/mm/access.h"
#include "kernel/signal.h"
#include "kernel/syscall/linux_abi.h"
#include "kernel/syscall/linux_compat.h"
#include "libc/user/syscall.h"

#define LC_ARGS                                                              \
    struct ARCH_REGS *r, uint64_t a, uint64_t b, uint64_t c, uint64_t d,      \
        uint64_t e, uint64_t f

int32_t sys_sigaction(int sig, const struct SYS_SIGACTION *act, struct SYS_SIGACTION *old);
int32_t sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int32_t sys_wait(int32_t *status);
uint32_t sys_brk(uint32_t addr);
uint64_t sys_sigreturn(struct ARCH_REGS *r);

static inline int user_ptr_ok(struct ARCH_REGS *r, uint64_t ptr, uint32_t len,
                              int wr) {
    return (r->cs & 3) != 3 || access_ok((const void *)(uintptr_t)ptr, len, wr);
}

static inline int copy_user_str(struct ARCH_REGS *r, char *dst, uint64_t ptr) {
    return (r->cs & 3) != 3 ||
           copy_str_from_user(dst, (const char *)(uintptr_t)ptr,
                              MAX_PATH_LEN) == 0;
}

__attribute__((noreturn));
int compat_fd_is_tty(int32_t fd);
int compat_fd_isdir(int32_t fd);
int ep_slot(int fd);
int evfd_slot(int fd);
int fill_sockaddr_in(struct ARCH_REGS *r, uint64_t addr, uint64_t addrlen_ptr, uint32_t ip, uint16_t port);
int io_fd_events(int fd, int want_read, int want_write);
int io_is_file_fd(int fd);
int io_wait(struct LINUX_POLLFD *fds, uint32_t n, int64_t timeout_ms);
int lc_close_extra(int32_t fd);
int lc_fdset_test(const uint8_t *set, int fd);
int lc_at_path(struct ARCH_REGS *r, int32_t dirfd, uint64_t uptr, char *out);
int sockaddr_in_parts(struct ARCH_REGS *r, uint64_t addr, uint64_t addrlen, uint32_t *ip, uint16_t *port);
int tfd_expired(int i);
int tfd_slot(int fd);
int unix_alloc_slot(void);
int unix_fd_slot(uint64_t fd);
int unix_peer_alive(int idx);
int32_t compat_dir_fd(const char *path);
int32_t compat_flags_linux2native(uint32_t lflags);
int32_t compat_fstat_linux(int32_t fd, uint64_t ub);
int32_t compat_ftruncate(int32_t fd, int32_t length);
int32_t compat_getdents64(int32_t fd, void *dirp, uint32_t count);
int32_t compat_getitimer(uint32_t which, uint64_t cur_val);
int32_t compat_getpgid(uint32_t pid);
int32_t compat_ioctl(int32_t fd, uint32_t cmd, uint64_t arg);
int32_t compat_openat(int32_t dirfd, const char *kpath, uint32_t lflags,
                      uint32_t mode);
int32_t compat_read(int32_t fd, void *buf, uint32_t count);
int32_t compat_readv(int32_t fd, struct LINUX_IOVEC *iov, int32_t iovcnt);
int32_t compat_set_thread_area(uint32_t base);
int32_t compat_setitimer(uint32_t which, uint64_t new_val, uint64_t old_val);
int32_t compat_setpgid(uint32_t pid, uint32_t pgid);
int32_t compat_stat_linux(const char *path, uint64_t ub, int follow);
int32_t compat_statfs_fill(uint64_t buf);
int32_t compat_sysinfo(void *buf);
int32_t compat_tcsets(uint32_t cmd, uint64_t arg);
int32_t compat_times(void *buf);
int32_t compat_uname(void *buf);
int32_t compat_wait4(int32_t *status_out);
int32_t compat_write(int32_t fd, const void *buf, uint32_t count);
int32_t sys_compat_writev(int32_t fd, struct LINUX_IOVEC *iov, int32_t iovcnt);
int32_t unix_recv(int idx, void *buf, uint32_t len);
int32_t unix_send(int idx, const void *buf, uint32_t len);
int64_t lc0_open(LC_ARGS);
int64_t lc0_writev(LC_ARGS);
int64_t lc_accept(LC_ARGS);
int64_t lc_access(LC_ARGS);
int64_t lc_arch_prctl(LC_ARGS);
int64_t lc_bind(LC_ARGS);
int64_t lc_brk(LC_ARGS);
int64_t lc_chdir(LC_ARGS);
int64_t lc_chmod(LC_ARGS);
int64_t lc_chown(LC_ARGS);
int64_t lc_clock_getres(LC_ARGS);
int64_t lc_clock_gettime(LC_ARGS);
int64_t lc_clone(LC_ARGS);
int64_t lc_close(LC_ARGS);
int64_t lc_connect(LC_ARGS);
int64_t lc_dup(LC_ARGS);
int64_t lc_dup2(LC_ARGS);
int64_t lc_dup3(LC_ARGS);
int64_t lc_epoll_create1(LC_ARGS);
int64_t lc_epoll_ctl(LC_ARGS);
int64_t lc_epoll_pwait(LC_ARGS);
int64_t lc_epoll_wait(LC_ARGS);
int64_t lc_epoll_wait_common(struct ARCH_REGS *r, uint64_t a, uint64_t b, uint64_t c, int64_t timeout_ms);
int64_t lc_eventfd(LC_ARGS);
int64_t lc_eventfd2(LC_ARGS);
int64_t lc_eventfd_read(int i, void *buf, uint32_t count);
int64_t lc_eventfd_write(int i, const void *buf, uint32_t count);
int64_t lc_execve(LC_ARGS);
int64_t lc_exit(LC_ARGS);
int64_t lc_faccessat(LC_ARGS);
int64_t lc_fchmod(LC_ARGS);
int64_t lc_fchmodat(LC_ARGS);
int64_t lc_fchown(LC_ARGS);
int64_t lc_fchownat(LC_ARGS);
int64_t lc_fcntl(LC_ARGS);
int64_t lc_flock(LC_ARGS);
int64_t lc_fork(LC_ARGS);
int64_t lc_fstat(LC_ARGS);
int64_t lc_fstatfs(LC_ARGS);
int64_t lc_fsync(LC_ARGS);
int64_t lc_ftruncate(LC_ARGS);
int64_t lc_futex(LC_ARGS);
int64_t lc_getcwd(LC_ARGS);
int64_t lc_getdents64(LC_ARGS);
int64_t lc_getegid(LC_ARGS);
int64_t lc_geteuid(LC_ARGS);
int64_t lc_getgid(LC_ARGS);
int64_t lc_getgroups(LC_ARGS);
int64_t lc_getitimer(LC_ARGS);
int64_t lc_getpeername(LC_ARGS);
int64_t lc_getpgid(LC_ARGS);
int64_t lc_getpid(LC_ARGS);
int64_t lc_getppid(LC_ARGS);
int64_t lc_getrandom(LC_ARGS);
void flock_release_ino(uint32_t ino);
int64_t lc_copy_file_range(LC_ARGS);
int64_t lc_getresgid(LC_ARGS);
int64_t lc_getresuid(LC_ARGS);
int64_t lc_getrlimit(LC_ARGS);
int64_t lc_getrusage(LC_ARGS);
int64_t lc_getsid(LC_ARGS);
int64_t lc_getsockname(LC_ARGS);
int64_t lc_getsockopt(LC_ARGS);
int64_t lc_gettimeofday(LC_ARGS);
int64_t lc_getuid(LC_ARGS);
int64_t lc_ioctl(LC_ARGS);
int64_t lc_kill(LC_ARGS);
int64_t lc_lchown(LC_ARGS);
int64_t lc_listen(LC_ARGS);
int64_t lc_lseek(LC_ARGS);
int64_t lc_madvise(LC_ARGS);
int64_t lc_mkdir(LC_ARGS);
int64_t lc_mkdirat(LC_ARGS);
int64_t lc_mknod(LC_ARGS);
int64_t lc_mknodat(LC_ARGS);
int64_t lc_mmap(LC_ARGS);
int64_t lc_mprotect(LC_ARGS);
int64_t lc_munmap(LC_ARGS);
int64_t lc_nanosleep(LC_ARGS);
int64_t lc_clock_nanosleep(LC_ARGS);
int64_t lc_newfstatat(LC_ARGS);
int64_t lc_open(LC_ARGS);
int64_t lc_openat(LC_ARGS);
int64_t lc_pipe(LC_ARGS);
int64_t lc_pipe2(LC_ARGS);
int64_t lc_poll(LC_ARGS);
int64_t lc_poll_common(struct ARCH_REGS *r, uint64_t ufds, int32_t nfds, int64_t timeout_ms);
int64_t lc_ppoll(LC_ARGS);
int64_t lc_pread64(LC_ARGS);
int64_t lc_prlimit64(LC_ARGS);
int64_t lc_pselect6(LC_ARGS);
int64_t lc_read(LC_ARGS);
int64_t lc_readlink(LC_ARGS);
int64_t lc_readlinkat(LC_ARGS);
int64_t lc_readv(LC_ARGS);
int64_t lc_recvfrom(LC_ARGS);
int64_t lc_recvmsg(LC_ARGS);
int64_t lc_rename(LC_ARGS);
int64_t lc_renameat(LC_ARGS);
int64_t lc_renameat2(LC_ARGS);
int64_t lc_rmdir(LC_ARGS);
int64_t lc_rt_sigaction(LC_ARGS);
int64_t lc_rt_sigprocmask(LC_ARGS);
int64_t lc_rt_sigreturn(LC_ARGS);
int64_t lc_sched_yield(LC_ARGS);
int64_t lc_select(LC_ARGS);
int64_t lc_select_common(struct ARCH_REGS *r, int32_t nfds, uint64_t rd, uint64_t wr, uint64_t ex, int64_t timeout_ms);
int64_t lc_sendmsg(LC_ARGS);
int64_t lc_sendto(LC_ARGS);
int64_t lc_set_thread_area(LC_ARGS);
int64_t lc_set_tid_address(LC_ARGS);
int64_t lc_gettid(LC_ARGS);
int64_t lc_setgid(LC_ARGS);
int64_t lc_setgroups(LC_ARGS);
int64_t lc_setitimer(LC_ARGS);
int64_t lc_setpgid(LC_ARGS);
int64_t lc_setregid(LC_ARGS);
int64_t lc_setresgid(LC_ARGS);
int64_t lc_setresuid(LC_ARGS);
int64_t lc_setreuid(LC_ARGS);
int64_t lc_setrlimit(LC_ARGS);
int64_t lc_setsid(LC_ARGS);
int64_t lc_setsockopt(LC_ARGS);
int64_t lc_setuid(LC_ARGS);
int64_t lc_shutdown(LC_ARGS);
int64_t lc_sigaltstack(LC_ARGS);
int64_t lc_sigsuspend(LC_ARGS);
int64_t lc_socket(LC_ARGS);
int64_t lc_socketpair(LC_ARGS);
int64_t lc_stat(LC_ARGS);
int64_t lc_lstat(LC_ARGS);
int64_t lc_statfs(LC_ARGS);
int64_t lc_symlink(LC_ARGS);
int64_t lc_symlinkat(LC_ARGS);
int64_t lc_sysinfo(LC_ARGS);
int64_t lc_tgkill(LC_ARGS);
int64_t lc_timerfd_create(LC_ARGS);
int64_t lc_timerfd_gettime(LC_ARGS);
int64_t lc_timerfd_read(int i, void *buf, uint32_t count);
int64_t lc_timerfd_settime(LC_ARGS);
int64_t lc_timerfd_tick(int i);
int64_t lc_times(LC_ARGS);
int64_t lc_timespec_to_ms(struct ARCH_REGS *r, uint64_t ptr, int64_t *out);
int64_t lc_tkill(LC_ARGS);
int64_t lc_truncate(LC_ARGS);
int64_t lc_umask(LC_ARGS);
int64_t lc_uname(LC_ARGS);
int64_t lc_unlink(LC_ARGS);
int64_t lc_unlinkat(LC_ARGS);
int64_t lc_utimensat(LC_ARGS);
int64_t lc_wait4(LC_ARGS);
int64_t lc_waitid(LC_ARGS);
int64_t lc_write(LC_ARGS);
int64_t lc_writev(LC_ARGS);
uint32_t timeval_to_ticks(const struct LINUX_TIMEVAL *tv);
uint32_t unix_buf_read(int idx, uint8_t *dst, uint32_t len);
uint32_t unix_buf_write(int idx, const uint8_t *src, uint32_t len);
uint64_t lc_now_ms(void);
void compat_stat_fill(struct LINUX_STAT *ls, uint32_t ino, int64_t size, uint32_t mode, uint32_t uid, uint32_t gid);
void compat_tcgets(uint8_t *p);
void lc_fdset_clear_high(uint8_t *set, uint32_t bytes, int nfds);
void lc_fdset_set(uint8_t *set, int fd);
void lc_fill_rlimit(uint64_t res, struct LINUX_RLIMIT *rl);
void lc_seterrno(struct TASK *cur, int32_t val);
void ticks_to_timeval(struct LINUX_TIMEVAL *tv, uint32_t ticks);
void unix_close_slot(int idx);

#endif
