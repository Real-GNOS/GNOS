/*
 * cpuid.c — SYS_cpuid: run the CPUID instruction for user space. (GPLv2)
 *
 * CPUID is unprivileged, but a syscall wrapper is still useful: a program
 * can query through one call with NULL-skippable registers, and the kernel
 * can someday mask feature bits per process.  Sub-leaves follow the
 * ECX convention (only meaningful for a few leaves, harmless otherwise).
 */
#include "cpuid.h"
#include "vmm.h"

int64_t cpuid_syscall(uint64_t leaf, uint64_t subleaf, uint64_t a, uint64_t b,
                      uint64_t c, uint64_t d)
{
    uint32_t ea = (uint32_t)leaf, eb = 0, ec = (uint32_t)subleaf, ed = 0;

    asm volatile("cpuid"
                 : "=a"(ea), "=b"(eb), "=c"(ec), "=d"(ed)
                 : "a"(ea), "c"(ec));

    if (a && user_ptr_ok(a, 4))
        *(uint32_t *)(uintptr_t)a = ea;
    if (b && user_ptr_ok(b, 4))
        *(uint32_t *)(uintptr_t)b = eb;
    if (c && user_ptr_ok(c, 4))
        *(uint32_t *)(uintptr_t)c = ec;
    if (d && user_ptr_ok(d, 4))
        *(uint32_t *)(uintptr_t)d = ed;
    return 0;
}
