/* SPDX-License-Identifier: GPL-2.0 */
/*
 * process_vm_access.c — read and write another process's memory. (GPLv2)
 *
 * Debuggers, sandbox supervisors and some IPC use this instead of ptrace:
 * one call, no stops, and the target keeps running.  The copy walks the
 * target's page tables page by page (vmm_resolve) and copies through the
 * direct map, so it works on any address the target has mapped.
 */
#include <stdint.h>

#include "process_vm_access.h"
#include "proc.h"
#include "vmm.h"
#include "vfs.h"

typedef struct { uint64_t base, len; } iovec_t;

extern uint64_t g_hhdm;

/* Copy between the caller's buffer and `as` at `va`, page at a time. */
static int64_t xcopy(addrspace_t *as, uint64_t va, uint8_t *local,
                    uint32_t len, int writing)
{
    uint64_t done = 0;
    while (done < len) {
        uint64_t cur = va + done;
        uint32_t in_page = (uint32_t)(cur & 0xFFF);
        uint32_t n = len - (uint32_t)done;
        if (n > 4096 - in_page)
            n = 4096 - in_page;

        uint64_t phys = vmm_resolve(as, cur & ~0xFFFULL);
        if (!phys) {
            extern void dbg_puts(const char *);
            extern void dbg_puts_hex(uint64_t);
            dbg_puts("PVM: resolve fail va=");
            dbg_puts_hex(cur);
            dbg_puts(" as=");
            dbg_puts_hex((uint64_t)(uintptr_t)as);
            dbg_puts("\r\n");
            return done ? (int64_t)done : -E_FAULT;
        }
        uint8_t *remote = (uint8_t *)(uintptr_t)(phys + g_hhdm) + in_page;

        if (writing)
            memcpy(remote, local + done, n);
        else
            memcpy(local + done, remote, n);
        done += n;
    }
    return (int64_t)done;
}

static int64_t process_vm_common(uint64_t pid, uint64_t uliov, uint64_t liovcnt,
                                 uint64_t uriov, uint64_t riovcnt, uint64_t flags,
                                 int writing)
{
    if (flags || !liovcnt || !riovcnt || liovcnt > 1024 || riovcnt > 1024)
        return -E_INVAL;

    proc_t *target = proc_by_pid((int)pid);
    if (!target || !target->as)
        return -E_SRCH;
    if (target == proc_current())
        return -E_PERM;               /* use regular memory access */

    iovec_t *liov = (iovec_t *)(uintptr_t)uliov;
    iovec_t *riov = (iovec_t *)(uintptr_t)uriov;
    if (!user_ptr_ok(uliov, liovcnt * sizeof(iovec_t)) ||
        !user_ptr_ok(uriov, riovcnt * sizeof(iovec_t)))
        return -E_FAULT;

    int64_t total = 0;
    uint64_t li = 0, ri = 0, loff = 0, roff = 0;

    while (li < liovcnt && ri < riovcnt) {
        uint64_t lleft = liov[li].len - loff;
        uint64_t rleft = riov[ri].len - roff;
        uint32_t n = (uint32_t)((lleft < rleft) ? lleft : rleft);
        if (!n) {
            if (!lleft) { li++; loff = 0; }
            if (!rleft) { ri++; roff = 0; }
            continue;
        }
        if (!user_ptr_ok(liov[li].base, liov[li].len))
            return total ? total : -E_FAULT;

        int64_t r = xcopy(target->as, riov[ri].base + roff,
                          (uint8_t *)(uintptr_t)(liov[li].base + loff), n,
                          writing);
        if (r <= 0)
            return total ? total : (r < 0 ? r : total);
        total += r;
        loff += (uint64_t)r;
        roff += (uint64_t)r;
        if (loff >= liov[li].len) { li++; loff = 0; }
        if (roff >= riov[ri].len) { ri++; roff = 0; }
    }
    return total;
}

int64_t sys_process_vm_readv(uint64_t pid, uint64_t liov, uint64_t liovcnt,
                             uint64_t riov, uint64_t riovcnt, uint64_t flags)
{
    return process_vm_common(pid, liov, liovcnt, riov, riovcnt, flags, 0);
}

int64_t sys_process_vm_writev(uint64_t pid, uint64_t liov, uint64_t liovcnt,
                              uint64_t riov, uint64_t riovcnt, uint64_t flags)
{
    return process_vm_common(pid, liov, liovcnt, riov, riovcnt, flags, 1);
}
