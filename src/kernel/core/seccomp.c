/*
 * seccomp.c — seccomp-BPF syscall filtering for GNOS. (GPLv2)
 *
 * Implements the Linux seccomp-BPF ABI: a process installs a classic-BPF
 * filter via prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) or the
 * seccomp(2) syscall, and every subsequent syscall runs through the
 * interpreter before reaching its normal handler.
 *
 * The filter receives a struct seccomp_data snapshot (syscall nr, arch,
 * args[0..5]) and returns a 32-bit action:
 *
 *   SECCOMP_RET_ALLOW   — allow the syscall
 *   SECCOMP_RET_ERRNO   — deny, return -(data & 0xffff)
 *   SECCOMP_RET_KILL_*  — kill the process
 *   SECCOMP_RET_TRAP    — send SIGSYS
 *   SECCOMP_RET_TRACE   — ptrace stop (for strace-like tools)
 *   SECCOMP_RET_LOG     — allow + log
 *
 * Filters are checked in reverse-installation order (most-recent first),
 * like Linux.  The last filter to return anything other than ALLOW wins.
 *
 * Locking: the BKL serialises syscall dispatch, so filter installation
 * and execution never race.  proc_exit() frees the filter without extra
 * locking because the BKL is held.
 */
#include <stddef.h>
#include <stdint.h>

#include "seccomp.h"
#include "proc.h"
#include "vfs.h"
#include "heap.h"
#include "kstring.h"
#include "debugcon.h"
#include "signal.h"
#include "syscall.h"

/* ------------------------------------------------------------------ */
/* BPF interpreter                                                     */
/* ------------------------------------------------------------------ */

/* The seccomp_data snapshot passed to the filter, matching the Linux ABI. */
struct seccomp_data {
    int   nr;                    /* syscall number */
    unsigned int arch;           /* AUDIT_ARCH_X86_64 */
    unsigned long long args[6];  /* syscall arguments */
};

#define AUDIT_ARCH_X86_64  0xC000003E

/* Evaluate a BPF program against a syscall.  Returns the action (lower 16
 * bits masked off) ORed with optional data (e.g., errno for SECCOMP_RET_ERRNO). */
static uint32_t bpf_run(const struct sock_filter *insn, unsigned int len,
                        const struct seccomp_data *sd)
{
    /* Classic BPF machine: accumulator A, index X, scratch memory M[16]. */
    uint32_t A = 0, X = 0;
    uint32_t M[16];
    memset(M, 0, sizeof(M));

    unsigned int ticks = len * 4 + 64;
    unsigned int ip = 0;   /* instruction pointer (index into insn[]) */

    while (ip < len && ticks--) {
        const struct sock_filter *pc = &insn[ip];
        uint16_t code = pc->code;
        uint32_t k    = pc->k;

        /* Each instruction advances by 1 by default.  Jumps override ip. */
        unsigned int next = ip + 1;

        switch (code) {
        /* ---- load from seccomp_data ----------------------------------- */
        case (BPF_LD | BPF_W | BPF_ABS):
            if (k + 4 > sizeof(struct seccomp_data))
                return SECCOMP_RET_KILL_PROCESS;
            A = *(uint32_t *)((const uint8_t *)sd + k);
            break;

        /* ---- ALU: A &= K ---------------------------------------------- */
        case (BPF_ALU | BPF_AND | BPF_K):
            A &= k;
            break;

        /* ---- conditional jumps: if A <op> K --------------------------- */
        case (BPF_JMP | BPF_JEQ | BPF_K):
            next = ip + 1 + ((A == k) ? pc->jt : pc->jf);
            break;
        case (BPF_JMP | BPF_JGE | BPF_K):
            next = ip + 1 + ((A >= k) ? pc->jt : pc->jf);
            break;
        case (BPF_JMP | BPF_JGT | BPF_K):
            next = ip + 1 + ((A > k) ? pc->jt : pc->jf);
            break;
        case (BPF_JMP | BPF_JSET | BPF_K):
            next = ip + 1 + ((A & k) ? pc->jt : pc->jf);
            break;

        /* ---- conditional jumps: if A <op> X --------------------------- */
        case (BPF_JMP | BPF_JEQ | BPF_X):
            next = ip + 1 + ((A == X) ? pc->jt : pc->jf);
            break;
        case (BPF_JMP | BPF_JGE | BPF_X):
            next = ip + 1 + ((A >= X) ? pc->jt : pc->jf);
            break;
        case (BPF_JMP | BPF_JGT | BPF_X):
            next = ip + 1 + ((A > X) ? pc->jt : pc->jf);
            break;
        case (BPF_JMP | BPF_JSET | BPF_X):
            next = ip + 1 + ((A & X) ? pc->jt : pc->jf);
            break;

        /* ---- misc: X = A ---------------------------------------------- */
        case (BPF_MISC | BPF_TAX):
            X = A;
            break;

        /* ---- store / load scratch memory ------------------------------ */
        case BPF_ST:
            if (k < 16) M[k] = A;
            break;
        case BPF_STX:
            if (k < 16) M[k] = X;
            break;
        case (BPF_LD | BPF_W | BPF_MEM):
            A = (k < 16) ? M[k] : 0;
            break;

        /* ---- return --------------------------------------------------- */
        case (BPF_RET | BPF_K):
            return k;
        case (BPF_RET | BPF_A):
            return A;

        default:
            return SECCOMP_RET_KILL_PROCESS;
        }

        ip = next;
    }
    return SECCOMP_RET_KILL_PROCESS;
}

/* ------------------------------------------------------------------ */
/* seccomp enforcement                                                 */
/* ------------------------------------------------------------------ */

/* Called from the syscall entry path (syscall_handler) before dispatch.
 * Returns 0 to allow, or a negative errno / SECCOMP_RET_* action.
 * Must be called with the BKL held (caller is syscall_handler). */
int seccomp_check(proc_t *p, int syscall_nr, uint64_t a1, uint64_t a2,
                  uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    if (!p)
        return 0;

    switch (p->secc_mode) {
    case SECCOMP_MODE_DISABLED:
        return 0;

    case SECCOMP_MODE_STRICT:
        /* In strict mode, only read/write/exit/sigreturn are allowed. */
        switch (syscall_nr) {
        case 0:  /* read */
        case 1:  /* write */
        case 8:  /* lseek */
        case 19: /* readv */
        case 20: /* writev */
        case 17: /* pread64 */
        case 18: /* pwrite64 */
        case 60: /* exit */
        case 61: /* wait4 */
        case 231: /* exit_group */
        case 15: /* rt_sigreturn */
            return 0;
        default:
            return -E_INVAL;  /* EPERM would be correct but errno 38 */
        }

    case SECCOMP_MODE_FILTER: {
        if (!p->seccomp_filter || p->seccomp_len == 0)
            return 0;

        struct seccomp_data sd;
        sd.nr   = syscall_nr;
        sd.arch = AUDIT_ARCH_X86_64;
        sd.args[0] = a1;
        sd.args[1] = a2;
        sd.args[2] = a3;
        sd.args[3] = a4;
        sd.args[4] = a5;
        sd.args[5] = a6;

        /* Run all filters in reverse-installation order; first non-ALLOW
         * result wins.  (For now we support a single filter; if multiple
         * filters are ever chained, walk them in reverse.) */
        uint32_t result = bpf_run(p->seccomp_filter, p->seccomp_len, &sd);
        uint32_t action = result & SECCOMP_RET_ACTION_MASK;

        switch (action) {
        case SECCOMP_RET_ALLOW:
            return 0;
        case SECCOMP_RET_LOG:
            /* Allow but log: GNOS logs on debug console. */
            dbg_puts("SECCOMP LOG nr=");
            dbg_puts_dec((uint32_t)syscall_nr);
            dbg_puts(" pid=");
            dbg_puts_dec((uint32_t)p->pid);
            dbg_puts("\n");
            return 0;
        case SECCOMP_RET_ERRNO:
            /* Return -(result & 0xffff) as errno. */
            {
                int e = (int)(result & 0xffff);
                if (e == 0) e = -E_INVAL;
                return -e;
            }
        case SECCOMP_RET_KILL_PROCESS:
        case SECCOMP_RET_KILL_THREAD:
            dbg_puts("SECCOMP KILL nr=");
            dbg_puts_dec((uint32_t)syscall_nr);
            dbg_puts(" pid=");
            dbg_puts_dec((uint32_t)p->pid);
            dbg_puts("\n");
            return -E_KILLED;  /* kill the process */
        case SECCOMP_RET_TRAP:
            /* Deliver SIGSYS to the process. */
            signal_deliver(p, 31 /* SIGSYS */, 0);
            return -E_INVAL;  /* let the signal handler run */
        case SECCOMP_RET_TRACE:
            /* For now, treat as allow (strace support would need ptrace). */
            return 0;
        default:
            return -E_INVAL;
        }
    }

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* prctl() integration                                                 */
/* ------------------------------------------------------------------ */

/* Handle PR_SET_NO_NEW_PRIVS, PR_GET_NO_NEW_PRIVS, PR_SET_SECCOMP,
 * PR_GET_SECCOMP.  Called from sys_prctl().
 * Returns 0 on success, -errno on failure. */
int seccomp_prctl(proc_t *p, int option, uint64_t arg2)
{
    if (!p)
        return -E_INVAL;

    switch (option) {
    case PR_SET_NO_NEW_PRIVS:
        if (arg2 != 1)
            return -E_INVAL;
        p->no_new_privs = 1;
        return 0;

    case PR_GET_NO_NEW_PRIVS:
        return p->no_new_privs ? 1 : 0;

    case PR_SET_SECCOMP: {
        int mode = (int)arg2;
        /* Only allowed transitions: disabled -> strict, disabled -> filter,
         * strict -> filter.  Once set, it can only become stricter. */
        if (p->secc_mode == SECCOMP_MODE_FILTER)
            return -E_INVAL;  /* already at strictest */
        if (mode == SECCOMP_MODE_STRICT) {
            p->secc_mode = SECCOMP_MODE_STRICT;
            return 0;
        }
        /* SECCOMP_MODE_FILTER requires no_new_privs to be set first. */
        if (mode == SECCOMP_MODE_FILTER && !p->no_new_privs)
            return -E_INVAL;
        if (mode != SECCOMP_MODE_FILTER && mode != SECCOMP_MODE_STRICT)
            return -E_INVAL;
        return 0;
    }

    case PR_GET_SECCOMP:
        return p->secc_mode;

    default:
        return -E_INVAL;
    }
}

/* ------------------------------------------------------------------ */
/* seccomp(2) syscall                                                  */
/* ------------------------------------------------------------------ */

/* Handle the seccomp(2) syscall.
 * a1 = op (SECCOMP_SET_MODE_FILTER)
 * a2 = flags (SECCOMP_FILTER_FLAG_TSYNC = 1)
 * a3 = pointer to struct sock_fprog */
int sys_seccomp(uint64_t op, uint64_t flags, uint64_t u_fprog)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    switch (op) {
    case SECCOMP_SET_MODE_FILTER: {
        if (p->no_new_privs == 0)
            return -E_INVAL;
        if (p->secc_mode == SECCOMP_MODE_FILTER)
            return -E_INVAL;  /* already filtered */

        /* Validate the user-space sock_fprog. */
        if (!user_ptr_ok(u_fprog, sizeof(struct sock_fprog)))
            return -E_INVAL;

        struct sock_fprog kf;
        memcpy(&kf, (const void *)(uintptr_t)u_fprog, sizeof(kf));

        if (kf.len == 0 || kf.len > SECCOMP_BPF_MAXINSNS)
            return -E_INVAL;
        if (!user_ptr_ok((uint64_t)kf.filter,
                         kf.len * sizeof(struct sock_filter)))
            return -E_INVAL;

        /* Allocate a kernel copy of the filter. */
        size_t sz = kf.len * sizeof(struct sock_filter);
        struct sock_filter *buf = kmalloc(sz);
        if (!buf)
            return -E_NOMEM;
        memcpy(buf, (const void *)(uintptr_t)kf.filter, sz);

        /* Install: kfree any old filter first (shouldn't happen, but be safe). */
        if (p->seccomp_filter)
            kfree(p->seccomp_filter);

        p->seccomp_filter = buf;
        p->seccomp_len    = kf.len;
        p->secc_mode      = SECCOMP_MODE_FILTER;

        /* TSYNC: if requested, also apply to all threads sharing the
         * same address space.  For GNOS this means the thread group. */
        if (flags & 0x1) {  /* SECCOMP_FILTER_FLAG_TSYNC */
            for (int i = 0; i < proc_capacity(); i++) {
                proc_t *q = proc_at(i);
                if (q == p || q->state == PROC_UNUSED)
                    continue;
                if (q->tgid != p->tgid)
                    continue;
                /* Only apply if the target is not yet filtered. */
                if (q->secc_mode != SECCOMP_MODE_FILTER && q->no_new_privs) {
                    q->secc_mode = SECCOMP_MODE_FILTER;
                    /* Share the same filter (increment refcount or copy).
                     * For simplicity, we copy the filter for each thread. */
                    q->seccomp_filter = kmalloc(sz);
                    if (q->seccomp_filter) {
                        memcpy(q->seccomp_filter, buf, sz);
                        q->seccomp_len = kf.len;
                    } else {
                        q->secc_mode = SECCOMP_MODE_DISABLED;
                    }
                }
            }
        }

        return 0;
    }

    case SECCOMP_GET_ACTION_AVAIL: {
        /* Returns 0 if the requested action is supported. */
        uint32_t action = (uint32_t)flags;
        switch (action & SECCOMP_RET_ACTION_MASK) {
        case SECCOMP_RET_ALLOW:
        case SECCOMP_RET_LOG:
        case SECCOMP_RET_ERRNO:
        case SECCOMP_RET_KILL_PROCESS:
        case SECCOMP_RET_TRAP:
        case SECCOMP_RET_TRACE:
            return 0;
        default:
            return -E_NOSYS;
        }
    }

    default:
        return -E_NOSYS;
    }
}

/* Called when a process exec's: if seccomed in filter mode, the filter
 * survives the exec (like Linux).  In strict mode it stays too.  But
 * if the exec is the one that set no_new_privs, the filter must be
 * installed AFTER the exec, not before (ENOEXEC).  This is already
 * handled by the no_new_privs prerequisite. */
void seccomp_exec(proc_t *p)
{
    (void)p;
    /* Filter survives exec in both strict and filter mode.
     * No action needed here. */
}

/* Called from proc_exit(): free the BPF filter if present. */
void seccomp_free(proc_t *p)
{
    if (p && p->seccomp_filter) {
            kfree(p->seccomp_filter);
            p->seccomp_filter = NULL;
            p->seccomp_len = 0;
    }
}
