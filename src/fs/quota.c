/*
 * quota.c — quotactl_fd(443). (GPLv2)
 *
 * The fd-based quota control call (Linux 5.14): instead of naming a block
 * special device, you hand it any open file and it acts on that mount.
 *
 * GNOS has no quota accounting at all -- no per-uid block or inode limits,
 * no quota files, nothing to turn on or off -- which is exactly the state
 * of a Linux filesystem whose superblock carries no quota operations, and
 * Linux answers such a filesystem with ENOTSUP.  So does this.  What is
 * validated first is the part a caller can get wrong: the descriptor, and
 * the command/type encoding.
 */
#include <stdint.h>

#include "quota.h"
#include "vfs.h"
#include "proc.h"

/* include/uapi/linux/quota.h */
#define Q_SYNC          0x800001
#define Q_QUOTAON       0x800002
#define Q_QUOTAOFF      0x800003
#define Q_GETFMT        0x800004
#define Q_GETINFO       0x800005
#define Q_SETINFO       0x800006
#define Q_GETQUOTA      0x800007
#define Q_SETQUOTA      0x800008
#define Q_GETNEXTQUOTA  0x800009

#define SUBCMDSHIFT     8
#define SUBCMDMASK      0x00ff

enum { USRQUOTA = 0, GRPQUOTA = 1, PRJQUOTA = 2 };

/* `cmd` is an int in Linux (QCMD() shifts 0x8nnnnn left by 8, which lands
 * outside int and is therefore passed sign-extended in the 64-bit
 * register); taking it as 32 bits is what makes the encoding decode. */
int64_t sys_quotactl_fd(uint64_t fd, uint32_t cmd, uint64_t id, uint64_t addr)
{
    (void)id; (void)addr;

    /* The descriptor must be open: quotactl_fd acts on its filesystem. */
    if (fd_handle((int)fd) < 0)
        return -E_BADF;

    uint32_t sub = (uint32_t)(cmd >> SUBCMDSHIFT);
    uint32_t type = (uint32_t)(cmd & SUBCMDMASK);

    switch (sub) {
    case Q_SYNC:
    case Q_QUOTAON:
    case Q_QUOTAOFF:
    case Q_GETFMT:
    case Q_GETINFO:
    case Q_SETINFO:
    case Q_GETQUOTA:
    case Q_SETQUOTA:
    case Q_GETNEXTQUOTA:
        break;
    default:
        return -E_INVAL;             /* not a quota subcommand */
    }
    if (type > PRJQUOTA)
        return -E_INVAL;             /* not user/group/project */

    /* No quota operations on this filesystem -- Linux's own answer when a
     * superblock has none.  Refusing is better than answering Q_GETQUOTA
     * with zeroes, which would read as "quotas are on and unlimited". */
    return -E_OPNOTSUPP;
}
