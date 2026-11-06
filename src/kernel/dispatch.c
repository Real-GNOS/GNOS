/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dispatch.c — the Linux-style syscall dispatch table. (GPLv2)
 *
 * Everything a user process can ask the kernel to do arrives here with the
 * Linux/x86-64 register convention: number in RAX, arguments in RDI, RSI,
 * RDX, R10, R8, R9, result back in RAX, failures as a negative errno.
 *
 * The old giant switch is gone.  sys_call_table[] is a plain array of
 * function pointers indexed by syscall number, filled from the single
 * __SYSCALL list shared with the handler prototypes in syscall_impl.h.
 * syscall_handler() is our do_syscall_64: it runs the ptrace/seccomp
 * prologue, indexes the table, and runs the shared syscall-exit tail.
 */
#include <stddef.h>
#include <stdint.h>

#include "syscall_impl.h"
#include "syscall.h"
#include "proc.h"
#include "ptrace.h"
#include "seccomp.h"
#include "vfs.h"
#include "panic.h"
#include "idt.h"
#include "gdt.h"
#include "debugcon.h"

/* The `syscall` instruction lands here; defined in isr.asm.  It builds a
 * regs_t frame and calls syscall_handler(), exactly like the int 0x80 path. */
extern uint8_t syscall_entry[];

static void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* One bit per syscall number, set the first time that number is refused. */
static uint64_t g_nosys_logged[8];

/* Pass 2: designated initializers.  No explicit size -- C sizes the array
 * to the largest SYS_* index + 1 (1004 today), leaving every hole NULL. */
#define __SYSCALL(nr, fn) [nr] = fn,
static const sys_call_ptr_t sys_call_table[] = {
#include "syscall_list.h"
};
#undef __SYSCALL

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
_Static_assert(ARRAY_SIZE(sys_call_table) == SYS_cpuid + 1,
               "sys_call_table must cover the highest SYS_* number");

void syscall_handler(regs_t *r)
{
    uint64_t nr = r->rax;
    uint64_t a1 = r->rdi;
    uint64_t a2 = r->rsi;
    uint64_t a3 = r->rdx;

    proc_t *p = proc_current();
    int64_t ret;

    /* TEMPORARY Xorg debugging: a syscall must run on its own task's slot. */
    proc_kstack_audit("SYS");

    /* Remembered for SA_RESTART: by the time signals are looked at, RAX holds
     * the result rather than the call number. */
    if (p)
        p->syscall_nr = (int64_t)nr;

    /* A tracee in PTRACE_SYSCALL mode stops before the call dispatches:
     * the tracer sees (and may rewrite) the original register image. */
    if (p) {
        ptrace_syscall_enter(r, nr);
        nr = r->rax; /* whatever the tracer left in RAX */
    }

    /* ---- seccomp-BPF filter check ------------------------------------
     * If this process has a seccomp filter, run it before dispatching the
     * syscall.  The filter may deny the call (returning -errno, killing
     * the process, or delivering SIGSYS). */
    if (p && p->secc_mode != SECCOMP_MODE_DISABLED) {
        int sc = seccomp_check(p, (int)nr, a1, a2, a3, r->r10, r->r8, r->r9);
        if (sc != 0) {
            if (sc == -E_KILLED) {
                /* Kill the process.  proc_exit with signal 31 (SIGSYS). */
                p->term_sig = 31;
                proc_exit(128 + 31);
                return;
            }
            ret    = sc;
            r->rax = (uint64_t)ret;
            goto done;
        }
    }

    if (nr < ARRAY_SIZE(sys_call_table) && sys_call_table[nr]) {
        ret = sys_call_table[nr](r);
        /* execve() success and rt_sigreturn() already own the register
         * frame: skip the RAX write and the whole syscall-exit tail. */
        if (ret == SYS_FRAME_REPLACED)
            return;
    } else {
        /* Once per syscall number, not once per call.  A libc that retries in
         * a loop on ENOSYS -- which is exactly what happens when a wait
         * primitive is missing -- otherwise turns the debug console into the
         * slowest part of the system: one boot with rt_sigsuspend missing
         * wrote six megabytes of this single line. */
        if (nr < 512 && !(g_nosys_logged[nr >> 6] & (1ULL << (nr & 63)))) {
            g_nosys_logged[nr >> 6] |= 1ULL << (nr & 63);
            dbg_puts("GNOS: unimplemented syscall ");
            dbg_puts_dec((uint32_t)nr);
            dbg_puts("\r\n");
        }
        ret = -E_NOSYS;
    }

#ifdef SYSTRACE
    if (nr == 9 || nr == 10 || nr == 12 || nr == 158 || nr == 218 || nr == 0 || nr == 1 ||
        nr == 19 || nr == 20 || nr == 7 || nr == 29 || nr == 43 || nr == 202 || nr == 232 ||
        nr == 257 || nr == 57 || nr == 217 || nr == 16 || nr == 8 || nr == 59 || nr == 61 ||
        nr == 270 || nr == 23 || nr == 281 || nr == 35 || nr == 230 || nr == 2 || nr == 3 ||
        nr == 5 || nr == 11 || nr == 13 || nr == 14 || nr == 6 || nr == 4 || nr == 89 || nr == 90 ||
        nr == 41 || nr == 42 || nr == 45 || nr == 46 || nr == 47) {
        dbg_puts("SY p=");
        dbg_puts_dec((uint32_t)(proc_current() ? proc_current()->pid : 0));
        dbg_puts(" nr=");
        dbg_puts_dec((uint32_t)nr);
        dbg_puts(" a1=");
        dbg_puts_hex(a1);
        dbg_puts(" a2=");
        dbg_puts_hex(a2);
        dbg_puts(" a3=");
        dbg_puts_hex(a3);
        dbg_puts(" ret=");
        dbg_puts_hex((uint64_t)(int64_t)ret);
        dbg_puts("\n");
    }
#endif
done:
    r->rax = (uint64_t)ret;

    /* Ring buffer of the last few syscalls, for the fault dumper: when a
     * user process dies on a NULL deref we want to see what it just did. */
    {
        proc_t *sp = proc_current();
        if (sp) {
            uint8_t i           = sp->sys_hist_idx & 7;
            sp->sys_hist_nr[i]  = (uint16_t)nr;
            sp->sys_hist_ret[i] = (int32_t)ret;
            sp->sys_hist_idx++;
        }
    }

    /* ... and once after it returns, where the exit stop's register image
     * shows the result in RAX. */
    ptrace_syscall_exit(r);
}

void syscall_init_cpu(void)
{
    /* Enable the `syscall` instruction.  These MSRs are per-core: an AP that
     * skips this takes #UD on the first user `syscall`.  We return to user
     * mode with iretq, so only the SYSCALL half of IA32_STAR matters: it
     * supplies the kernel code selector the CPU switches into on entry. */
    wrmsr(0xC0000081,                 /* IA32_STAR: syscall CS = kernel,  */
          ((uint64_t)SEL_KCODE << 32) /* sysret CS field (unused here)    */
              | ((uint64_t)SEL_UCODE << 48));
    wrmsr(0xC0000082, (uint64_t)syscall_entry); /* IA32_LSTAR */
    /* EFER must be a READ-MODIFY-WRITE: under KVM-SVM with NPT the
     * hypervisor owns EFER.SVME (bit 12) and rejects a write that clears
     * it -- a blind 0x801 store #GPs on KVM (TCG, where EFER reads as 0,
     * never noticed).  Set SCE, add NXE when the CPU reports NX. */
    {
        uint64_t efer;
        uint32_t a = 0, d = 0;
        uint32_t maxext = 0;
        asm volatile("cpuid" : "=a"(maxext) : "a"(0x80000000) : "ebx", "ecx", "edx");
        if (maxext >= 0x80000001u)
            asm volatile("cpuid" : "=a"(a), "=d"(d) : "a"(0x80000001), "c"(0u) : "ebx");
        asm volatile("rdmsr" : "=A"(efer) : "c"(0xC0000080u));
        efer |= 0x1; /* SCE: enable syscall */
        if ((d & (1u << 20)) && maxext >= 0x80000001u)
            efer |= 0x800; /* NXE when NX exists */
        wrmsr(0xC0000080, efer);
    }
    /* IA32_FMASK left at 0: the stub clears IF itself, and leaving RFLAGS
     * untouched means the saved R11 still carries the user's IF for iretq. */
}

void syscall_init(void)
{
    syscall_install(syscall_handler);
    syscall_init_cpu();

    dbg_puts("GNOS: syscall gate installed (int 0x80 + syscall)\r\n");
}
