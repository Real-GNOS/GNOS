#ifndef COREZ_LINUX_ABI_H
#define COREZ_LINUX_ABI_H

#include <stdint.h>

#define SYS_LINUX_read 0
#define SYS_LINUX_write 1
#define SYS_LINUX_open 2
#define SYS_LINUX_close 3
#define SYS_LINUX_stat 4
#define SYS_LINUX_fstat 5
#define SYS_LINUX_lstat 6
#define SYS_LINUX_poll 7
#define SYS_LINUX_lseek 8
#define SYS_LINUX_mmap 9
#define SYS_LINUX_mprotect 10
#define SYS_LINUX_munmap 11
#define SYS_LINUX_brk 12
#define SYS_LINUX_rt_sigaction 13
#define SYS_LINUX_rt_sigprocmask 14
#define SYS_LINUX_rt_sigreturn 15
#define SYS_LINUX_ioctl 16
#define SYS_LINUX_pread64 17
#define SYS_LINUX_pwrite64 18
#define SYS_LINUX_readv 19
#define SYS_LINUX_writev 20
#define SYS_LINUX_access 21
#define SYS_LINUX_pipe 22
#define SYS_LINUX_select 23
#define SYS_LINUX_sched_yield 24
#define SYS_LINUX_dup 32
#define SYS_LINUX_dup2 33
#define SYS_LINUX_nanosleep 35
#define SYS_LINUX_getpid 39
#define SYS_LINUX_socket 41
#define SYS_LINUX_connect 42
#define SYS_LINUX_accept 43
#define SYS_LINUX_sendto 44
#define SYS_LINUX_recvfrom 45
#define SYS_LINUX_sendmsg 46
#define SYS_LINUX_recvmsg 47
#define SYS_LINUX_shutdown 48
#define SYS_LINUX_bind 49
#define SYS_LINUX_listen 50
#define SYS_LINUX_getsockname 51
#define SYS_LINUX_getpeername 52
#define SYS_LINUX_socketpair 53
#define SYS_LINUX_setsockopt 54
#define SYS_LINUX_getsockopt 55
#define SYS_LINUX_clone 56
#define SYS_LINUX_fork 57
#define SYS_LINUX_vfork 58
#define SYS_LINUX_execve 59
#define SYS_LINUX_exit 60
#define SYS_LINUX_wait4 61
#define SYS_LINUX_kill 62
#define SYS_LINUX_uname 63
#define SYS_LINUX_fcntl 72
#define SYS_LINUX_ftruncate 77
#define SYS_LINUX_getcwd 79
#define SYS_LINUX_chdir 80
#define SYS_LINUX_rename 82
#define SYS_LINUX_mkdir 83
#define SYS_LINUX_rmdir 84
#define SYS_LINUX_creat 85
#define SYS_LINUX_link 86
#define SYS_LINUX_unlink 87
#define SYS_LINUX_symlink 88
#define SYS_LINUX_readlink 89
#define SYS_LINUX_chmod 90
#define SYS_LINUX_chown 92
#define SYS_LINUX_gettimeofday 96
#define SYS_LINUX_getuid 102
#define SYS_LINUX_getgid 104
#define SYS_LINUX_geteuid 107
#define SYS_LINUX_getegid 108
#define SYS_LINUX_getppid 110
#define SYS_LINUX_setpgid 109
#define SYS_LINUX_getpgid 121
#define SYS_LINUX_setsid 112
#define SYS_LINUX_fchmod 91
#define SYS_LINUX_chown 92
#define SYS_LINUX_fchown 93
#define SYS_LINUX_lchown 94
#define SYS_LINUX_umask 95
#define SYS_LINUX_getuid 102
#define SYS_LINUX_setuid 105
#define SYS_LINUX_getgid 104
#define SYS_LINUX_setgid 106
#define SYS_LINUX_geteuid 107
#define SYS_LINUX_getegid 108
#define SYS_LINUX_setreuid 113
#define SYS_LINUX_setregid 114
#define SYS_LINUX_getgroups 115
#define SYS_LINUX_setgroups 116
#define SYS_LINUX_setresuid 117
#define SYS_LINUX_getresuid 118
#define SYS_LINUX_setresgid 119
#define SYS_LINUX_getresgid 120
#define SYS_LINUX_fchownat 260
#define SYS_LINUX_fchmodat 268
#define SYS_LINUX_tkill 200
#define SYS_LINUX_times 153
#define SYS_LINUX_arch_prctl 158
#define SYS_LINUX_sysinfo 179
#define SYS_LINUX_futex 202
#define SYS_LINUX_getdents64 217
#define SYS_LINUX_set_tid_address 218
#define SYS_LINUX_clock_gettime 228
#define SYS_LINUX_clock_getres 229
#define SYS_LINUX_clock_nanosleep 230
#define SYS_LINUX_exit_group 231
#define SYS_LINUX_set_thread_area 205
#define SYS_LINUX_openat 257
#define SYS_LINUX_mkdirat 258
#define SYS_LINUX_symlink 88
#define SYS_LINUX_mknod 133
#define SYS_LINUX_mknodat 259
#define SYS_LINUX_newfstatat 262
#define SYS_LINUX_unlinkat 263
#define SYS_LINUX_renameat 264
#define SYS_LINUX_readlinkat 267
#define SYS_LINUX_symlinkat 266
#define SYS_LINUX_faccessat 269
#define SYS_LINUX_dup3 292
#define SYS_LINUX_pipe2 293
#define SYS_LINUX_renameat2 316
#define SYS_LINUX_getrandom 318

#define SYS_LINUX_madvise 28
#define SYS_LINUX_flock 73
#define SYS_LINUX_fsync 74
#define SYS_LINUX_fdatasync 75
#define SYS_LINUX_truncate 76
#define SYS_LINUX_getrlimit 97
#define SYS_LINUX_setrlimit 160
#define SYS_LINUX_prlimit64 302
#define SYS_LINUX_pselect6 270
#define SYS_LINUX_ppoll 271
#define SYS_LINUX_epoll_create 213
#define SYS_LINUX_epoll_wait 232
#define SYS_LINUX_epoll_ctl 233
#define SYS_LINUX_epoll_pwait 281
#define SYS_LINUX_epoll_create1 291
#define SYS_LINUX_eventfd 284
#define SYS_LINUX_eventfd2 290
#define SYS_LINUX_timerfd_create 283
#define SYS_LINUX_timerfd_settime 286
#define SYS_LINUX_timerfd_gettime 287
#define SYS_LINUX_statfs 137
#define SYS_LINUX_fstatfs 138
#define SYS_LINUX_futex 202
#define SYS_LINUX_gettid 186
#define SYS_LINUX_membarrier 324
#define SYS_LINUX_rseq 334
#define SYS_LINUX_getrandom 318
#define SYS_LINUX_copy_file_range 326

#define LINUX_O_RDONLY 0
#define LINUX_O_WRONLY 1
#define LINUX_O_RDWR 2
#define LINUX_O_CREAT 0x40
#define LINUX_O_EXCL 0x80
#define LINUX_O_NOCTTY 0x100
#define LINUX_O_TRUNC 0x200
#define LINUX_O_APPEND 0x400
#define LINUX_O_NONBLOCK 0x800
#define LINUX_O_DIRECTORY 0x10000
#define LINUX_O_CLOEXEC 0x80000

#define LINUX_AT_FDCWD -100
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

#define LINUX_F_DUPFD 0
#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_FD_CLOEXEC 1

#define LINUX_SIGHUP 1
#define LINUX_SIGINT 2
#define LINUX_SIGQUIT 3
#define LINUX_SIGILL 4
#define LINUX_SIGABRT 6
#define LINUX_SIGFPE 8
#define LINUX_SIGKILL 9
#define LINUX_SIGSEGV 11
#define LINUX_SIGPIPE 13
#define LINUX_SIGALRM 14
#define LINUX_SIGTERM 15
#define LINUX_SIGCHLD 17
#define LINUX_SIGCONT 18
#define LINUX_SIGSTOP 19
#define LINUX_SIGTSTP 20
#define LINUX_SIGTTIN 21
#define LINUX_SIGTTOU 22
#define LINUX_SIGUSR1 10
#define LINUX_SIGUSR2 12

#define LINUX_SIG_BLOCK 0
#define LINUX_SIG_SETMASK 1
#define LINUX_SIG_UNBLOCK 2

#define LINUX_SA_NOCLDSTOP 1
#define LINUX_SA_NOCLDWAIT 2
#define LINUX_SA_SIGINFO 4
#define LINUX_SA_ONSTACK 0x08000000
#define LINUX_SA_RESTART 0x10000000
#define LINUX_SA_NODEFER 0x40000000
#define LINUX_SA_RESTORER 0x04000000

#define LINUX_WNOHANG 1
#define LINUX_WUNTRACED 2
#define LINUX_WCONTINUED 8

#define LINUX_WIFEXITED(s) (((s) & 0x7f) == 0)
#define LINUX_WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define LINUX_WTERMSIG(s) ((s) & 0x7f)
#define LINUX_WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define LINUX_WIFSTOPPED(s) (((s) & 0xff) == 0x7f)

#define LINUX_TCGETS 0x5401
#define LINUX_TCSETS 0x5402
#define LINUX_TCSETSW 0x5403
#define LINUX_TCSETSF 0x5404
#define LINUX_TCFLSH 0x540B
#define LINUX_TIOCGPGRP 0x540f
#define LINUX_TIOCSPGRP 0x5410
#define LINUX_TIOCSCTTY 0x540E
#define LINUX_TIOCGWINSZ 0x5413
#define LINUX_TIOCSWINSZ 0x5414
#define LINUX_FIONREAD 0x541b
#define LINUX_FIONBIO 0x5421

#define LINUX_NCCS 32
#define LINUX_VTIME 5
#define LINUX_VMIN 6

#define LINUX_MAP_SHARED 1
#define LINUX_MAP_PRIVATE 2
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_PROT_READ 1
#define LINUX_PROT_WRITE 2
#define LINUX_PROT_EXEC 4

#define LINUX_EPERM 1
#define LINUX_ENOENT 2
#define LINUX_ESRCH 3
#define LINUX_EINTR 4
#define LINUX_EIO 5
#define LINUX_E2BIG 7
#define LINUX_ENOEXEC 8
#define LINUX_EBADF 9
#define LINUX_EAGAIN 11
#define LINUX_ENOMEM 12
#define LINUX_EACCES 13
#define LINUX_EFAULT 14
#define LINUX_EBUSY 16
#define LINUX_EEXIST 17
#define LINUX_ENOTDIR 20
#define LINUX_EISDIR 21
#define LINUX_EINVAL 22
#define LINUX_ENOLCK 37
#define LINUX_ENFILE 23
#define LINUX_EMFILE 24
#define LINUX_ENOSPC 28
#define LINUX_ESPIPE 29
#define LINUX_EPIPE 32
#define LINUX_EAFNOSUPPORT 97
#define LINUX_ERANGE 34
#define LINUX_ENAMETOOLONG 36
#define LINUX_UTIME_NOW 0x3FFFFFFEu
#define LINUX_UTIME_OMIT 0x3FFFFFFDu
#define SYS_LINUX_utimensat 280
#define LINUX_ENOSYS 38
#define LINUX_ENOTTY 25
#define LINUX_ECHILD 10
#define LINUX_ENOTEMPTY 39
#define LINUX_ELOOP 40
#define LINUX_ENOTSOCK 88
#define LINUX_EOPNOTSUPP 95
#define LINUX_EWOULDBLOCK 11
#define LINUX_EXDEV 18
#define LINUX_ETIMEDOUT 110
#define LINUX_ENOTCONN 107

#define LINUX_NAME_MAX 255
#define LINUX_PATH_MAX 4096

#define LINUX_DT_FIFO 1
#define LINUX_DT_CHR 2
#define LINUX_DT_DIR 4
#define LINUX_DT_BLK 6
#define LINUX_DT_REG 8
#define LINUX_DT_LNK 10

#define LINUX_S_IFMT 0xF000u
#define LINUX_S_IFIFO 0x1000u
#define LINUX_S_IFCHR 0x2000u
#define LINUX_S_IFDIR 0x4000u
#define LINUX_S_IFREG 0x8000u
#define LINUX_S_IFLNK 0xA000u
#define LINUX_S_IRWXU 0700u
#define LINUX_S_IRUSR 0400u
#define LINUX_S_IWUSR 0200u
#define LINUX_S_IXUSR 0100u
#define LINUX_S_IRWXG 070u
#define LINUX_S_IRGRP 040u
#define LINUX_S_IWGRP 020u
#define LINUX_S_IXGRP 010u
#define LINUX_S_IRWXO 07u
#define LINUX_S_IROTH 04u
#define LINUX_S_IWOTH 02u
#define LINUX_S_IXOTH 01u

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_CLOCK_BOOTTIME 7

#define LINUX_ICRNL 0x100
#define LINUX_IXON 0x400
#define LINUX_OPOST 1
#define LINUX_ONLCR 4
#define LINUX_CS8 0x30
#define LINUX_ISIG 1
#define LINUX_ICANON 2
#define LINUX_ECHO 8
#define LINUX_IEXTEN 0x8000

struct LINUX_IOVEC {
    void *iov_base;
    uint64_t iov_len;
};

struct LINUX_MSGHDR {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t ctrl;
    uint64_t ctrllen;
    int32_t flags;
};

struct LINUX_DIRENT64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

struct LINUX_SIGACTION {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

struct LINUX_TERMIOS {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[LINUX_NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct LINUX_WINSIZE {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

struct LINUX_TIMESPEC {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct LINUX_TIMEVAL {
    int64_t tv_sec;
    int64_t tv_usec;
};

#define SYS_LINUX_getsid 124
#define SYS_LINUX_sigaltstack 131
#define SYS_LINUX_getitimer 36
#define SYS_LINUX_setitimer 38
#define SYS_LINUX_getrusage 98
#define SYS_LINUX_rt_sigsuspend 130
#define SYS_LINUX_tgkill 234
#define SYS_LINUX_statfs 137
#define SYS_LINUX_fstatfs 138
#define SYS_LINUX_waitid 247

#define LINUX_MINSIGSTKSZ 2048
#define LINUX_SS_ONSTACK 1
#define LINUX_SS_DISABLE 2

#define LINUX_ITIMER_REAL 0
#define LINUX_ITIMER_VIRTUAL 1
#define LINUX_ITIMER_PROF 2
#define LINUX_TIMER_ABSTIME 1

#define LINUX_RUSAGE_SELF 0
#define LINUX_RUSAGE_CHILDREN -1

#define LINUX_P_ALL 0
#define LINUX_P_PID 1
#define LINUX_P_PGID 2

#define LINUX_WSTOPPED 2u
#define LINUX_WEXITED 4u
#define LINUX_WNOWAIT 0x1000000u

#define LINUX_CLD_EXITED 1
#define LINUX_CLD_KILLED 2
#define LINUX_CLD_DUMPED 3
#define LINUX_CLD_TRAPPED 4
#define LINUX_CLD_STOPPED 5
#define LINUX_CLD_CONTINUED 6

#define LINUX_EXT2_SUPER_MAGIC 0xEF53u

struct LINUX_SIGINFO {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t si_pad0;
    int32_t si_pid;
    int32_t si_uid;
    int32_t si_status;
    int32_t si_pad1;
    int64_t si_utime;
    int64_t si_stime;
    uint8_t si_pad[80];
};

struct LINUX_STACK_T {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t ss_pad;
    uint64_t ss_size;
};

struct LINUX_ITIMERVAL {
    struct LINUX_TIMEVAL it_interval;
    struct LINUX_TIMEVAL it_value;
};

struct LINUX_RUSAGE {
    struct LINUX_TIMEVAL ru_utime;
    struct LINUX_TIMEVAL ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};

struct LINUX_STATFS {
    int64_t f_type;
    int64_t f_bsize;
    int64_t f_blocks;
    int64_t f_bfree;
    int64_t f_bavail;
    int64_t f_files;
    int64_t f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen;
    int64_t f_frsize;
    int64_t f_flags;
    int64_t f_spare[4];
};

#define LINUX_POLLIN 0x001
#define LINUX_POLLPRI 0x002
#define LINUX_POLLOUT 0x004
#define LINUX_POLLERR 0x008
#define LINUX_POLLHUP 0x010
#define LINUX_POLLNVAL 0x020

struct LINUX_POLLFD {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct LINUX_RLIMIT {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct LINUX_EPOLL_EVENT {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct LINUX_ITIMERSPEC {
    struct LINUX_TIMESPEC it_interval;
    struct LINUX_TIMESPEC it_value;
};

#define LINUX_RLIMIT_CPU 0
#define LINUX_RLIMIT_FSIZE 1
#define LINUX_RLIMIT_DATA 2
#define LINUX_RLIMIT_STACK 3
#define LINUX_RLIMIT_CORE 4
#define LINUX_RLIMIT_RSS 5
#define LINUX_RLIMIT_NPROC 6
#define LINUX_RLIMIT_NOFILE 7
#define LINUX_RLIMIT_MEMLOCK 8
#define LINUX_RLIMIT_AS 9
#define LINUX_RLIMIT_LOCKS 10
#define LINUX_RLIMIT_SIGPENDING 11
#define LINUX_RLIMIT_MSGQUEUE 12
#define LINUX_RLIMIT_NICE 13
#define LINUX_RLIMIT_RTPRIO 14
#define LINUX_RLIMIT_RTTIME 15
#define LINUX_RLIMIT_NLIMITS 16

#define LINUX_EPOLL_CTL_ADD 1
#define LINUX_EPOLL_CTL_DEL 2
#define LINUX_EPOLL_CTL_MOD 3
#define LINUX_EPOLL_CLOEXEC 0x80000

#define LINUX_TFD_TIMER_ABSTIME 1
#define LINUX_TFD_NONBLOCK 0x800
#define LINUX_EFD_SEMAPHORE 1
#define LINUX_EFD_NONBLOCK 0x800
#define LINUX_EFD_CLOEXEC 0x80000

struct LINUX_STAT {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    struct LINUX_TIMESPEC st_atim;
    struct LINUX_TIMESPEC st_mtim;
    struct LINUX_TIMESPEC st_ctim;
    int64_t unused[3];
};

struct LINUX_SYSINFO {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char _f[20 - 16 - 4];
};

struct LINUX_TMS {
    int64_t utime;
    int64_t stime;
    int64_t cutime;
    int64_t cstime;
};

struct LINUX_UTSNAME {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

#endif
