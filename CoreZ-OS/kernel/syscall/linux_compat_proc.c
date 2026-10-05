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








#include "kernel/syscall/lc_internal.h"
int32_t compat_setpgid(uint32_t pid, uint32_t pgid) {
    int32_t spid = (int32_t)pid;
    int32_t spgid = (int32_t)pgid;
    if (spid < 0 || spgid < 0)
        return -LINUX_EINVAL;
    struct TASK *t = spid ? pid2thread(spid) : current;
    if (t == NULL || t->status == TASK_DIED)
        return -LINUX_ESRCH;
    t->pgid = spgid ? (uint32_t)spgid : t->pid;
    return 0;
}
int32_t compat_getpgid(uint32_t pid) {
    int32_t spid = (int32_t)pid;
    if (spid < 0)
        return -LINUX_EINVAL;
    struct TASK *t = spid ? pid2thread(spid) : current;
    if (t == NULL)
        return -LINUX_ESRCH;
    return (int32_t)(t->pgid ? t->pgid : t->pid);
}
void ticks_to_timeval(struct LINUX_TIMEVAL *tv, uint32_t ticks) {
    uint64_t us = (uint64_t)ticks * (1000000ull / PIT_HZ);
    tv->tv_sec = (int64_t)(us / 1000000ull);
    tv->tv_usec = (int64_t)(us % 1000000ull);
}
uint32_t timeval_to_ticks(const struct LINUX_TIMEVAL *tv) {
    uint64_t us = (uint64_t)tv->tv_sec * 1000000ull +
                  (uint64_t)tv->tv_usec;
    uint64_t tick_us = 1000000ull / PIT_HZ;
    return (uint32_t)((us + tick_us - 1) / tick_us);
}
int32_t compat_setitimer(uint32_t which, uint64_t new_val,
                                uint64_t old_val) {
    if (which != LINUX_ITIMER_REAL)
        return -LINUX_EINVAL;
    struct TASK *cur = current;
    if (old_val) {
        struct LINUX_ITIMERVAL o;
        memset(&o, 0, sizeof(o));
        if (cur->itimer_expire) {
            uint32_t rem = cur->itimer_expire > tick
                               ? cur->itimer_expire - tick
                               : 0;
            ticks_to_timeval(&o.it_value, rem);
            ticks_to_timeval(&o.it_interval,
                             (uint32_t)cur->itimer_interval);
        }
        memcpy((void *)(uintptr_t)old_val, &o, sizeof(o));
    }
    if (new_val) {
        struct LINUX_ITIMERVAL n;
        memcpy(&n, (const void *)(uintptr_t)new_val, sizeof(n));
        cur->itimer_interval = timeval_to_ticks(&n.it_interval);
        uint32_t v = timeval_to_ticks(&n.it_value);
        cur->itimer_expire = v ? tick + v : 0;
        if (cur->itimer_interval && !v)
            cur->itimer_expire = 0;
    } else {
        cur->itimer_expire = 0;
        cur->itimer_interval = 0;
    }
    return 0;
}
int32_t compat_getitimer(uint32_t which, uint64_t cur_val) {
    if (which != LINUX_ITIMER_REAL || !cur_val)
        return which != LINUX_ITIMER_REAL ? -LINUX_EINVAL : 0;
    struct TASK *cur = current;
    struct LINUX_ITIMERVAL o;
    memset(&o, 0, sizeof(o));
    if (cur->itimer_expire) {
        uint32_t rem = cur->itimer_expire > tick ? cur->itimer_expire - tick
                                                 : 0;
        ticks_to_timeval(&o.it_value, rem);
        ticks_to_timeval(&o.it_interval, (uint32_t)cur->itimer_interval);
    }
    memcpy((void *)(uintptr_t)cur_val, &o, sizeof(o));
    return 0;
}
int64_t lc_setitimer(LC_ARGS) {
    (void)r;
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_ITIMERVAL), 0))
        return -LINUX_EFAULT;
    if (c && !user_ptr_ok(r, c, sizeof(struct LINUX_ITIMERVAL), 1))
        return -LINUX_EFAULT;
    return compat_setitimer((uint32_t)a, b, c);
}
int64_t lc_getitimer(LC_ARGS) {
    (void)r;
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_ITIMERVAL), 1))
        return -LINUX_EFAULT;
    return compat_getitimer((uint32_t)a, b);
}
int64_t lc_getrusage(LC_ARGS) {
    (void)a;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_RUSAGE), 1))
        return -LINUX_EFAULT;
    struct LINUX_RUSAGE ru;
    memset(&ru, 0, sizeof(ru));
    if ((int32_t)a == LINUX_RUSAGE_SELF)
        ticks_to_timeval(&ru.ru_utime, current->elapsed_ticks);
    memcpy((void *)(uintptr_t)b, &ru, sizeof(ru));
    return 0;
}
int64_t lc_getsid(LC_ARGS) {
    (void)r;
    struct TASK *t = a ? pid2thread((int32_t)a) : current;
    if (t == NULL)
        return -LINUX_ESRCH;
    return (int64_t)(t->sid ? t->sid : t->pid);
}
int64_t lc_umask(LC_ARGS) {
    (void)r;
    struct TASK *cur = current;
    uint32_t old = cur->umask;
    cur->umask = (uint32_t)a & 0o7777u;
    return (int64_t)old;
}
#define LC_GETID(name, field)                                                \
    int64_t name(LC_ARGS) {                                                  \
        (void)r; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;       \
        return (int64_t)current->field;                                      \
    }

LC_GETID(lc_getuid, uid)
LC_GETID(lc_getgid, gid)
LC_GETID(lc_geteuid, euid)
LC_GETID(lc_getegid, egid)
int64_t lc_setuid(LC_ARGS) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t v = (uint32_t)a;
    if (cur->euid == 0) {
        cur->uid = cur->euid = cur->suid = v;
        return 0;
    }
    if (v == cur->uid || v == cur->suid) {
        cur->euid = v;
        return 0;
    }
    return -LINUX_EPERM;
}
int64_t lc_setgid(LC_ARGS) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t v = (uint32_t)a;
    if (cur->egid == 0) {
        cur->gid = cur->egid = cur->sgid = v;
        return 0;
    }
    if (v == cur->gid || v == cur->sgid) {
        cur->egid = v;
        return 0;
    }
    return -LINUX_EPERM;
}
int64_t lc_setreuid(LC_ARGS) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t ru = (a == (uint64_t)-1) ? cur->uid : (uint32_t)a;
    uint32_t eu = (b == (uint64_t)-1) ? cur->euid : (uint32_t)b;
    if (cur->euid != 0 && ((ru != cur->uid && ru != cur->suid) ||
                           (eu != cur->uid && eu != cur->euid &&
                            eu != cur->suid)))
        return -LINUX_EPERM;
    cur->uid = ru;
    cur->euid = eu;
    if (cur->euid != 0 || (b != (uint64_t)-1 && eu != cur->uid))
        cur->suid = eu;
    return 0;
}
int64_t lc_setregid(LC_ARGS) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t rg = (a == (uint64_t)-1) ? cur->gid : (uint32_t)a;
    uint32_t eg = (b == (uint64_t)-1) ? cur->egid : (uint32_t)b;
    if (cur->egid != 0 && ((rg != cur->gid && rg != cur->sgid) ||
                           (eg != cur->gid && eg != cur->egid &&
                            eg != cur->sgid)))
        return -LINUX_EPERM;
    cur->gid = rg;
    cur->egid = eg;
    if (cur->egid != 0 || (b != (uint64_t)-1 && eg != cur->gid))
        cur->sgid = eg;
    return 0;
}
int64_t lc_setresuid(LC_ARGS) {
    (void)r; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t ru = (a == (uint64_t)-1) ? cur->uid : (uint32_t)a;
    uint32_t eu = (b == (uint64_t)-1) ? cur->euid : (uint32_t)b;
    uint32_t su = (c == (uint64_t)-1) ? cur->suid : (uint32_t)c;
    if (cur->euid != 0 &&
        ((ru != cur->uid && ru != cur->euid && ru != cur->suid) ||
         (eu != cur->uid && eu != cur->euid && eu != cur->suid) ||
         (su != cur->uid && su != cur->euid && su != cur->suid)))
        return -LINUX_EPERM;
    cur->uid = ru;
    cur->euid = eu;
    cur->suid = su;
    return 0;
}
int64_t lc_setresgid(LC_ARGS) {
    (void)r; (void)d; (void)e; (void)f;
    struct TASK *cur = current;
    uint32_t rg = (a == (uint64_t)-1) ? cur->gid : (uint32_t)a;
    uint32_t eg = (b == (uint64_t)-1) ? cur->egid : (uint32_t)b;
    uint32_t sg = (c == (uint64_t)-1) ? cur->sgid : (uint32_t)c;
    if (cur->egid != 0 &&
        ((rg != cur->gid && rg != cur->egid && rg != cur->sgid) ||
         (eg != cur->gid && eg != cur->egid && eg != cur->sgid) ||
         (sg != cur->gid && sg != cur->egid && sg != cur->sgid)))
        return -LINUX_EPERM;
    cur->gid = rg;
    cur->egid = eg;
    cur->sgid = sg;
    return 0;
}
int64_t lc_getresuid(LC_ARGS) {
    (void)r; (void)d; (void)e; (void)f;
    if (!user_ptr_ok(r, a, 4, 1) || !user_ptr_ok(r, b, 4, 1) ||
        !user_ptr_ok(r, c, 4, 1))
        return -LINUX_EFAULT;
    *(uint32_t *)(uintptr_t)a = current->uid;
    *(uint32_t *)(uintptr_t)b = current->euid;
    *(uint32_t *)(uintptr_t)c = current->suid;
    return 0;
}
int64_t lc_getresgid(LC_ARGS) {
    (void)r; (void)d; (void)e; (void)f;
    if (!user_ptr_ok(r, a, 4, 1) || !user_ptr_ok(r, b, 4, 1) ||
        !user_ptr_ok(r, c, 4, 1))
        return -LINUX_EFAULT;
    *(uint32_t *)(uintptr_t)a = current->gid;
    *(uint32_t *)(uintptr_t)b = current->egid;
    *(uint32_t *)(uintptr_t)c = current->sgid;
    return 0;
}
int64_t lc_getgroups(LC_ARGS) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0;
}
int64_t lc_setgroups(LC_ARGS) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    if (current->egid != 0 && current->euid != 0)
        return -LINUX_EPERM;
    return 0;
}
int64_t lc_tkill(LC_ARGS) {
    (void)r;
    return sys_kill((int)a, (int)b) < 0 ? -LINUX_EINVAL : 0;
}
int64_t lc_tgkill(LC_ARGS) {
    (void)r;
    (void)a;
    return sys_kill((int)b, (int)c) < 0 ? -LINUX_EINVAL : 0;
}
int64_t lc_setsid(LC_ARGS) {
    (void)r;
    (void)a;
    struct TASK *cur = current;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct TASK *t = &task_table[i];
        if (t == cur || !t->slot_used || t->status == TASK_DIED)
            continue;
        if (t->pgid == cur->pid) {
            return -LINUX_EPERM;
        }
    }
    cur->sid = cur->pid;
    cur->pgid = cur->pid;
    return (int64_t)cur->pid;
}
int64_t lc_waitid(LC_ARGS) {
    if (c && !user_ptr_ok(r, c, sizeof(struct LINUX_SIGINFO), 1))
        return -LINUX_EFAULT;
    int rc = sys_waitid((int)a, (int32_t)b, (struct LINUX_SIGINFO *)(uintptr_t)c,
                        (uint32_t)d);
    return rc == 0 ? 0 : -LINUX_ECHILD;
}
int64_t lc_sigaltstack(LC_ARGS) {
    struct TASK *cur = current;
   
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_STACK_T), 1))
        return -LINUX_EFAULT;
    if (b) {
        struct LINUX_STACK_T o;
        o.ss_sp = cur->sigalt_sp;
        o.ss_flags = cur->sigalt_sp ? 0 : LINUX_SS_DISABLE;
        o.ss_pad = 0;
        o.ss_size = cur->sigalt_size;
        memcpy((void *)(uintptr_t)b, &o, sizeof(o));
    }
    if (a) {
        if (!user_ptr_ok(r, a, sizeof(struct LINUX_STACK_T), 0))
            return -LINUX_EFAULT;
        struct LINUX_STACK_T n;
        memcpy(&n, (const void *)(uintptr_t)a, sizeof(n));
        if (n.ss_flags & ~LINUX_SS_DISABLE)
            return -LINUX_EINVAL;
        if (n.ss_flags & LINUX_SS_DISABLE) {
            cur->sigalt_sp = 0;
            cur->sigalt_size = 0;
            cur->sigalt_flags = LINUX_SS_DISABLE;
        } else {
            if (n.ss_size < LINUX_MINSIGSTKSZ)
                return -LINUX_ENOMEM;
            cur->sigalt_sp = n.ss_sp;
            cur->sigalt_size = (uint32_t)n.ss_size;
            cur->sigalt_flags = 0;
        }
    }
    return 0;
}
int64_t lc_sigsuspend(LC_ARGS) {
    if (!user_ptr_ok(r, a, 4, 0))
        return -LINUX_EFAULT;
    uint32_t mask;
    memcpy(&mask, (const void *)(uintptr_t)a, 4);
    struct TASK *cur = current;
    uint32_t old = cur->signal_mask;
    cur->signal_mask = (mask << 1) & ~((1u << SIGKILL) | (1u << SIGSTOP));
    for (;;) {
        if (cur->signal_pending & ~cur->signal_mask)
            break;
        thread_block_with_status(TASK_WAITING);
    }
    check_pending_signals(r);
    cur->signal_mask = old;
    return -LINUX_EINTR;
}
int32_t compat_wait4(int32_t *status_out) {
    int32_t st = 0;
    int32_t pid = sys_wait(&st);
    if (pid < 0)
        return -LINUX_ECHILD;
    if (status_out) {
        int32_t s8 = st & 0xff;
        uint32_t lst;
        if (s8 >= 128)
            lst = (uint32_t)(s8 - 128);
        else
            lst = (uint32_t)s8 << 8;
        *(int32_t *)status_out = (int32_t)lst;
    }
    return pid;
}
int32_t compat_uname(void *buf) {
    struct LINUX_UTSNAME u;
    memset(&u, 0, sizeof(u));
    memcpy(u.sysname, "Linux", 6);
    memcpy(u.nodename, "corez", 6);
    memcpy(u.release, "5.10.0-corez", 13);
    memcpy(u.version, "#1 CoreZOS SMP", 16);
    memcpy(u.machine, "x86_64", 7);
    memcpy(u.domainname, "(none)", 7);
    memcpy(buf, &u, sizeof(u));
    return 0;
}
int32_t compat_sysinfo(void *buf) {
    struct LINUX_SYSINFO si;
    memset(&si, 0, sizeof(si));
    si.uptime = (int64_t)(tick / PIT_HZ);
    si.totalram = (uint64_t)kernel_pool.pool_size;
    si.freeram = (uint64_t)kernel_pool_free_count() * PAGE_SIZE;
    si.mem_unit = 1;
    si.procs = 1;
    memcpy(buf, &si, sizeof(si));
    return 0;
}
int32_t compat_times(void *buf) {
    if (buf) {
        struct LINUX_TMS t;
        memset(&t, 0, sizeof(t));
        t.utime = (int64_t)current->elapsed_ticks;
        memcpy(buf, &t, sizeof(t));
    }
    return (int32_t)tick;
}


int32_t compat_set_thread_area(uint32_t base) {
    if (base == 0 || !user_range_writable(base, sizeof(int32_t)))
        return -LINUX_EFAULT;
    struct TASK *cur = current;
    cur->tls_base = base;
    cur->tls_selector = SELECTOR_TLS;
    cur->tls_msr = 0;
    tls_desc_set_base(base);
    return 0;
}
int64_t lc_getpid(LC_ARGS) {
    (void)r;
    return (int64_t)current->pid;
}
int64_t lc_getppid(LC_ARGS) {
    (void)r;
    struct TASK *cur = current;
    return cur->parent_pid >= 0 ? (int64_t)cur->parent_pid : 0;
}
int64_t lc_exit(LC_ARGS) {
    (void)r;
    sys_exit((int32_t)a);
    return 0;
}

int64_t lc_brk(LC_ARGS) {
    (void)r;
    return (int64_t)sys_brk((uint32_t)a);
}
int64_t lc_mmap(LC_ARGS) {
    (void)r;
    struct SYS_MMAP_ARGS m = {(uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d,
                          (uint32_t)e,
                          (uint32_t)(f & ~(uint32_t)(PAGE_SIZE - 1u))};
    int64_t ret = (int32_t)sys_mmap(&m);
    return ret;
}
int64_t lc_munmap(LC_ARGS) {
    (void)r;
    return sys_munmap((uint32_t)a, (uint32_t)b);
}
int64_t lc_set_thread_area(LC_ARGS) {
    (void)r;
    return compat_set_thread_area((uint32_t)a);
}
int64_t lc_gettid(LC_ARGS) {
    (void)r;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    return (int64_t)current->pid;
}
int64_t lc_set_tid_address(LC_ARGS) {
    (void)r;
    struct TASK *cur = current;
    if (user_ptr_ok(r, a, 4, 1))
        cur->clear_child_tid = a;
    return (int64_t)cur->pid;
}
int64_t lc_kill(LC_ARGS) {
    (void)r;
    return sys_kill((int)a, (int)b);
}
int64_t lc_futex(LC_ARGS) {
    if (!user_ptr_ok(r, a, 4, 0))
        return -LINUX_EFAULT;
    uint32_t op = (uint32_t)b & 0x7f;
    if (d && (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET)) {
        if (!user_ptr_ok(r, d, sizeof(struct LINUX_TIMESPEC), 0))
            return -LINUX_EFAULT;
    }
    return sys_futex(a, b, c, d, e, f);
}
int64_t lc_gettimeofday(LC_ARGS) {
    struct LINUX_TIMEVAL tv;
    tv.tv_sec = (int64_t)rtc_unix_time();
    tv.tv_usec = 0;
    if (a && !user_ptr_ok(r, a, sizeof(tv), 1))
        return -LINUX_EFAULT;
    if (b && !user_ptr_ok(r, b, 16, 1))
        return -LINUX_EFAULT;
    if (a)
        memcpy((void *)(uintptr_t)a, &tv, sizeof(tv));
    if (b)
        memset((void *)(uintptr_t)b, 0, 16);
    return 0;
}
int64_t lc_nanosleep(LC_ARGS) {
    struct LINUX_TIMESPEC req;
    memset(&req, 0, sizeof(req));
    if (!a || !user_ptr_ok(r, a, sizeof(req), 0))
        return -LINUX_EFAULT;
    memcpy(&req, (const void *)(uintptr_t)a, sizeof(req));
    int64_t ms = req.tv_sec * 1000 + req.tv_nsec / 1000000;
    if (ms < 0)
        return -LINUX_EINVAL;
    if (ms > 0x7fffffff)
        ms = 0x7fffffff;
    if (mtime_sleep_interruptible((uint32_t)ms) == -EINTR) {
        if (b && user_ptr_ok(r, b, sizeof(struct LINUX_TIMESPEC), 1)) {
            uint64_t ns = (uint64_t)current->sleep_left *
                          (uint64_t)(1000000000u / PIT_HZ);
            struct LINUX_TIMESPEC rem;
            rem.tv_sec = (int64_t)(ns / 1000000000ull);
            rem.tv_nsec = (int64_t)(ns % 1000000000ull);
            memcpy((void *)(uintptr_t)b, &rem, sizeof(rem));
        }
        return -LINUX_EINTR;
    }
    return 0;
}
int64_t lc_clock_nanosleep(LC_ARGS) {
    int32_t clk = (int32_t)a;
    int32_t flags = (int32_t)b;
    struct LINUX_TIMESPEC req;
    if (clk != 0 && clk != 1)
        return -LINUX_EINVAL;
    if (flags & ~LINUX_TIMER_ABSTIME)
        return -LINUX_EINVAL;
    memset(&req, 0, sizeof(req));
    if (!c || !user_ptr_ok(r, c, sizeof(req), 0))
        return -LINUX_EFAULT;
    memcpy(&req, (const void *)(uintptr_t)c, sizeof(req));
    if (req.tv_nsec < 0 || req.tv_nsec >= 1000000000)
        return -LINUX_EINVAL;
    int64_t ms;
    if (flags & LINUX_TIMER_ABSTIME) {
        int64_t now_ns;
        if (clk == 0) {
            now_ns = (int64_t)rtc_unix_time() * 1000000000LL;
        } else {
            now_ns = (int64_t)(tick / (uint32_t)PIT_HZ) * 1000000000LL +
                     (int64_t)(tick % (uint32_t)PIT_HZ) *
                         (1000000000LL / (int64_t)PIT_HZ);
        }
        int64_t delta = req.tv_sec * 1000000000LL + req.tv_nsec - now_ns;
        if (delta <= 0)
            return 0;
        ms = delta / 1000000;
        if (ms == 0)
            ms = 1;
    } else {
        ms = req.tv_sec * 1000 + req.tv_nsec / 1000000;
    }
    if (ms < 0)
        return -LINUX_EINVAL;
    if (ms > 0x7fffffff)
        ms = 0x7fffffff;
    if (mtime_sleep_interruptible((uint32_t)ms) == -EINTR) {
        if (!(flags & LINUX_TIMER_ABSTIME) && d &&
            user_ptr_ok(r, d, sizeof(struct LINUX_TIMESPEC), 1)) {
            uint64_t ns = (uint64_t)current->sleep_left *
                          (uint64_t)(1000000000u / PIT_HZ);
            struct LINUX_TIMESPEC rem;
            rem.tv_sec = (int64_t)(ns / 1000000000ull);
            rem.tv_nsec = (int64_t)(ns % 1000000000ull);
            memcpy((void *)(uintptr_t)d, &rem, sizeof(rem));
        }
        return -LINUX_EINTR;
    }
    return 0;
}
int64_t lc_clock_gettime(LC_ARGS) {
    struct LINUX_TIMESPEC ts;
    if ((int32_t)a == 0) {
        ts.tv_sec = (int64_t)rtc_unix_time();
        ts.tv_nsec = 0;
    } else {
        ts.tv_sec = (int64_t)(tick / PIT_HZ);
        ts.tv_nsec = (int64_t)((tick % PIT_HZ) * (1000000000 / PIT_HZ));
    }
    if (b && !user_ptr_ok(r, b, sizeof(ts), 1))
        return -LINUX_EFAULT;
    if (b)
        memcpy((void *)(uintptr_t)b, &ts, sizeof(ts));
    return 0;
}
int64_t lc_clock_getres(LC_ARGS) {
    if (b && !user_ptr_ok(r, b, sizeof(struct LINUX_TIMESPEC), 1))
        return -LINUX_EFAULT;
    if (b) {
        struct LINUX_TIMESPEC res;
        res.tv_sec = 0;
        res.tv_nsec = 1000000000 / PIT_HZ;
        memcpy((void *)(uintptr_t)b, &res, sizeof(res));
    }
    return 0;
}
int64_t lc_mprotect(LC_ARGS) {
    (void)r;
    return sys_mprotect(a, b, c);
}
int64_t lc_rt_sigaction(LC_ARGS) {
    int32_t sig = (int32_t)a;
    if (sig <= 0 || sig >= 32)
        return -LINUX_EINVAL;
    struct LINUX_SIGACTION lsa;
    memset(&lsa, 0, sizeof(lsa));
    if (b && !user_ptr_ok(r, b, sizeof(lsa), 0)) {
        return -LINUX_EFAULT;
    }
    if (b)
        memcpy(&lsa, (const void *)(uintptr_t)b, sizeof(lsa));
    struct SYS_SIGACTION nat;
    memset(&nat, 0, sizeof(nat));
    nat.sa_handler = (void (*)(int))(uintptr_t)lsa.sa_handler;
    nat.sa_mask = (uint32_t)(lsa.sa_mask << 1);
    nat.sa_flags = (uint32_t)lsa.sa_flags;
    nat.sa_restorer = (void *)(uintptr_t)lsa.sa_restorer;
    if (d != 8)
        return -LINUX_EINVAL;
    struct SYS_SIGACTION oldnat;
    memset(&oldnat, 0, sizeof(oldnat));
    int rr = sys_sigaction(sig, b ? &nat : NULL, &oldnat);
    if (rr < 0)
        return rr;
    if (c) {
        if (!user_ptr_ok(r, c, sizeof(struct LINUX_SIGACTION), 1))
            return -LINUX_EFAULT;
        struct LINUX_SIGACTION oldl;
        memset(&oldl, 0, sizeof(oldl));
        oldl.sa_handler = (uint64_t)(uintptr_t)oldnat.sa_handler;
        oldl.sa_mask = (uint64_t)oldnat.sa_mask >> 1;
        oldl.sa_flags = oldnat.sa_flags;
        oldl.sa_restorer = (uint64_t)(uintptr_t)oldnat.sa_restorer;
        memcpy((void *)(uintptr_t)c, &oldl, sizeof(oldl));
    }
    return 0;
}
int64_t lc_rt_sigprocmask(LC_ARGS) {
    sigset_t kset = 0;
    if (b && !user_ptr_ok(r, b, 8, 0))
        return -LINUX_EFAULT;
    if (c && !user_ptr_ok(r, c, 8, 1))
        return -LINUX_EFAULT;
    if (b) {
        uint8_t in[8];
        memset(in, 0, sizeof(in));
        memcpy(in, (const void *)(uintptr_t)b, 8);
        memcpy(&kset, in, sizeof(kset));
        kset <<= 1;
    }
    sigset_t oset = 0;
    int32_t rr =
        sys_sigprocmask((int32_t)a, b ? &kset : NULL, c ? &oset : NULL);
    if (rr < 0)
        return rr;
    if (c) {
        uint8_t out[8];
        sigset_t mout = (sigset_t)oset >> 1;
        memset(out, 0, sizeof(out));
        memcpy(out, &mout, sizeof(mout));
        memcpy((void *)(uintptr_t)c, out, 8);
    }
    return 0;
}
int64_t lc_wait4(LC_ARGS) {
    if (b && !user_ptr_ok(r, b, 4, 1))
        return -LINUX_EFAULT;
    return compat_wait4((int32_t *)b);
}
int64_t lc_uname(LC_ARGS) {
    if (!user_ptr_ok(r, a, sizeof(struct LINUX_UTSNAME), 1))
        return -LINUX_EFAULT;
    return compat_uname((void *)a);
}
int64_t lc_sysinfo(LC_ARGS) {
    if (!user_ptr_ok(r, a, sizeof(struct LINUX_SYSINFO), 1))
        return -LINUX_EFAULT;
    return compat_sysinfo((void *)a);
}
int64_t lc_times(LC_ARGS) {
    if (a && !user_ptr_ok(r, a, sizeof(struct LINUX_TMS), 1))
        return -LINUX_EFAULT;
    return (int64_t)(uint32_t)compat_times((void *)a);
}
int64_t lc_rt_sigreturn(LC_ARGS) {
    return (int64_t)sys_sigreturn(r);
}
int64_t lc_setpgid(LC_ARGS) {
    (void)r;
    return compat_setpgid(a, b);
}
int64_t lc_getpgid(LC_ARGS) {
    (void)r;
    return compat_getpgid(a);
}
int64_t lc_arch_prctl(LC_ARGS) {
    (void)r;
    const uint64_t MSR_FS_BASE = 0xC0000100;
    const uint64_t MSR_GS_BASE = 0xC0000101;
    struct TASK *cur = current;
    switch (a) {
    case 0x1002u:
        cur->tls_base = (uint32_t)b;
        cur->tls_msr = 1;
        asm_wrmsr(MSR_FS_BASE, b);
        return 0;
    case 0x1001u:
        cur->tls_base = (uint32_t)b;
        asm_wrmsr(MSR_GS_BASE, b);
        return 0;
    case 0x1003u:
        return (int64_t)(uint32_t)asm_rdmsr(MSR_FS_BASE);
    case 0x1004u:
        return (int64_t)(uint32_t)asm_rdmsr(MSR_GS_BASE);
    default:
        return -LINUX_EINVAL;
    }
}
int64_t lc_sched_yield(LC_ARGS) {
    (void)r;
    return 0;
}
int64_t lc_execve(LC_ARGS) {
    char kpath[MAX_PATH_LEN];
    if (!copy_user_str(r, kpath, a))
        return -LINUX_EFAULT;
    return sys_execve(kpath, (const char **)(uintptr_t)b,
                      (const char **)(uintptr_t)c, r);
}
int64_t lc_fork(LC_ARGS) {
    return sys_fork(r);
}
int64_t lc_clone(LC_ARGS) {
    (void)d;
    (void)f;
    uint32_t flags = (uint32_t)a;
    if ((flags & CLONE_VM) == 0)
        return sys_clone_ex(flags, (uint32_t)b, (uint32_t)e, r);
    if ((flags & CLONE_THREAD) != 0) {
        int64_t tpid = (int64_t)sys_clone_ex(flags, (uint32_t)b, (uint32_t)e, r);
        if (tpid > 0 && (flags & CLONE_PARENT_SETTID) != 0 && c != 0) {
            if (user_ptr_ok(r, c, 4, 1))
                *(volatile int32_t *)(uintptr_t)c = (int32_t)tpid;
        }
        return tpid;
    }
    int64_t pid = (int64_t)sys_clone_ex(flags & ~(uint32_t)CLONE_VM,
                                        (uint32_t)b, (uint32_t)e, r);
    if (pid > 0 && (flags & CLONE_PARENT_SETTID) != 0 && c != 0) {
        if (user_ptr_ok(r, c, 4, 1))
            *(volatile int32_t *)(uintptr_t)c = (int32_t)pid;
    }
    return pid;
}
int64_t lc_getrandom(LC_ARGS) {
    if (b == 0)
        return 0;
    if (!user_ptr_ok(r, a, (uint32_t)b, 1))
        return -LINUX_EFAULT;
    uint8_t *p = (uint8_t *)(uintptr_t)a;
    uint32_t n = (uint32_t)b;
    while (n >= 8) {
        uint64_t v = rand_u64();
        memcpy(p, &v, 8);
        p += 8;
        n -= 8;
    }
    if (n) {
        uint64_t v = rand_u64();
        memcpy(p, &v, n);
    }
    return (int64_t)b;
}
void lc_fill_rlimit(uint64_t res, struct LINUX_RLIMIT *rl) {
    switch (res) {
    case LINUX_RLIMIT_NOFILE:
        rl->rlim_cur = 1024;
        rl->rlim_max = 4096;
        break;
    case LINUX_RLIMIT_STACK:
        rl->rlim_cur = 8u * 1024u * 1024u;
        rl->rlim_max = 8u * 1024u * 1024u;
        break;
    case LINUX_RLIMIT_DATA:
    case LINUX_RLIMIT_AS:
        rl->rlim_cur = 1ull << 32;
        rl->rlim_max = 1ull << 32;
        break;
    case LINUX_RLIMIT_NPROC:
        rl->rlim_cur = MAX_TASKS;
        rl->rlim_max = MAX_TASKS;
        break;
    default:
        rl->rlim_cur = 0xFFFFFFFFFFFFFFFFull;
        rl->rlim_max = 0xFFFFFFFFFFFFFFFFull;
        break;
    }
}
int64_t lc_getrlimit(LC_ARGS) {
    (void)c; (void)d; (void)e; (void)f;
    if (b == 0 || !user_ptr_ok(r, b, sizeof(struct LINUX_RLIMIT), 1))
        return -LINUX_EFAULT;
    struct LINUX_RLIMIT rl;
    lc_fill_rlimit(a, &rl);
    memcpy((void *)(uintptr_t)b, &rl, sizeof(rl));
    return 0;
}
int64_t lc_setrlimit(LC_ARGS) {
    (void)a; (void)c; (void)d; (void)e; (void)f;
    if (b == 0 || !user_ptr_ok(r, b, sizeof(struct LINUX_RLIMIT), 0))
        return -LINUX_EFAULT;
    return 0;
}
int64_t lc_prlimit64(LC_ARGS) {
    (void)e; (void)f;
    if (a != 0 && (int32_t)a != (int32_t)current->pid && (int32_t)a != -1)
        return -LINUX_EPERM;
    if (d != 0) {
        if (!user_ptr_ok(r, d, sizeof(struct LINUX_RLIMIT), 1))
            return -LINUX_EFAULT;
        struct LINUX_RLIMIT rl;
        lc_fill_rlimit(b, &rl);
        memcpy((void *)(uintptr_t)d, &rl, sizeof(rl));
    }
    return 0;
}
int64_t lc_madvise(LC_ARGS) {
    (void)c; (void)d; (void)e; (void)f;
    if (a == 0)
        return -LINUX_EINVAL;
    if (!user_ptr_ok(r, a, (uint32_t)b, 1))
        return -LINUX_ENOMEM;
    return 0;
}
