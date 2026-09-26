/*
 * landlock.c — landlock_create_ruleset(444) / landlock_add_rule(445) /
 * landlock_restrict_self(446) and process_mrelease(448). (GPLv2)
 *
 * Landlock is an unprivileged sandbox: you describe which paths a process
 * may still touch and then lock yourself into it.  process_mrelease lets a
 * supervisor hurry along the reaping of a process the OOM killer has
 * already condemned.
 *
 * GNOS has neither an LSM hook framework nor an OOM reaper, so none of these
 * can do anything -- and the honest answer is ENOSYS, which is exactly what
 * Linux returns when LANDLOCK/that reaping path is not built in.  Callers
 * that only *try* to sandbox themselves (the documented usage: degrade
 * gracefully when landlock is unavailable) keep working.
 *
 * What is enforced is the argument contract, so a caller that got it wrong
 * hears EINVAL/EBADF rather than a silent nothing.
 */
#include <stdint.h>

#include "landlock.h"
#include "pidfd.h"
#include "proc.h"
#include "vfs.h"        /* errno values */

/* include/uapi/linux/landlock.h */
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)

enum landlock_rule_type {
    LANDLOCK_RULE_PATH_BENEATH = 1,
    LANDLOCK_RULE_NET_PORT,
    LANDLOCK_RULE_SCOPED,
};

#define RULESET_FD_MAGIC_UNUSED 0   /* no such descriptors exist here */

int64_t sys_landlock_create_ruleset(uint64_t attr, uint64_t size, uint64_t flags)
{
    (void)attr;
    if (flags & ~LANDLOCK_CREATE_RULESET_VERSION)
        return -E_INVAL;
    if (!size)
        return -E_INVAL;             /* Linux: EINVAL for a zero size */
    return -E_NOSYS;
}

int64_t sys_landlock_add_rule(uint64_t ruleset_fd, uint64_t rule_type,
                              uint64_t rule_attr, uint64_t flags)
{
    (void)rule_attr;
    if (flags)
        return -E_INVAL;
    if (rule_type < LANDLOCK_RULE_PATH_BENEATH ||
        rule_type > LANDLOCK_RULE_SCOPED)
        return -E_INVAL;
    /* There is no such thing as a ruleset descriptor here. */
    if (fd_handle((int)ruleset_fd) < 0)
        return -E_BADF;
    return -E_NOSYS;
}

int64_t sys_landlock_restrict_self(uint64_t ruleset_fd, uint64_t flags)
{
    if (flags)
        return -E_INVAL;
    if (fd_handle((int)ruleset_fd) < 0)
        return -E_BADF;
    return -E_NOSYS;
}

int64_t sys_process_mrelease(uint64_t pidfd, uint64_t flags)
{
    if (flags)
        return -E_INVAL;
    /* A real pidfd is required -- a stale one must read as EBADF, never as
     * "released" (which would let a supervisor believe it reclaimed memory
     * belonging to a process that may not even be the one it meant). */
    if (!pidfd_proc_of((int)pidfd))
        return -E_BADF;
    return -E_NOSYS;                 /* no OOM reaper, no reclaim pass */
}
