/*
 * secretmem.c — memfd_secret(447). (GPLv2)
 *
 * Linux's memfd_secret hands back a file whose pages are mapped in the
 * owning process only: they are *removed from the kernel's direct map*, so
 * neither stray kernel reads nor another process (nor a side channel
 * through the linear mapping) can see them.
 *
 * GNOS cannot offer that, and the reason is structural rather than a missing
 * feature flag: the direct map IS how this kernel reaches another address
 * space.  process_vm_readv/writev, ptrace and the futex path all resolve the
 * target's page tables and touch the memory through it.  A page hidden from
 * that map would break those calls, and a page merely *marked* secret while
 * still mapped would be a promise the kernel does not keep -- worse than
 * refusing.
 *
 * So this returns ENOSYS, which is exactly what Linux answers when
 * CONFIG_SECRETMEM is not enabled.  A caller that only *prefers* secret
 * memory (the documented usage) falls back to ordinary anonymous memory.
 */
#include <stdint.h>

#include "secretmem.h"
#include "vfs.h"        /* errno values */

int64_t sys_memfd_secret(uint64_t flags)
{
    /* Only flags == 0 is defined; anything else is a misuse. */
    if (flags)
        return -E_INVAL;
    return -E_NOSYS;
}
