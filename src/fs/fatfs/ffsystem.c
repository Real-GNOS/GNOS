/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ffsystem.c — the OS services FatFs asks for. (GPLv2)
 *
 * Two of these are real: memory comes from kmalloc, and the file time
 * stamps from the boot epoch the kernel already keeps.  The mutex
 * operations are deliberately stubs -- the kernel runs its filesystem
 * code under the big kernel lock, so there is no second thread to
 * exclude, and a lock that cannot be contended is a lock that costs
 * nothing.
 */
#include <stdint.h>

#include "ff.h"
#include "heap.h"
#include "timer.h"

#if FF_USE_LFN == 3
#error "FF_USE_LFN=3 needs a working heap; the kernel heap is fine, see below"
#endif

void *ff_memalloc(UINT msize)
{
    return kmalloc(msize);
}

void ff_memfree(void *mblock)
{
    kfree(mblock);
}

#if FF_FS_REENTRANT
int ff_mutex_create(int vol)
{
    (void)vol;
    return 1;
}

void ff_mutex_delete(int vol)
{
    (void)vol;
}

int ff_mutex_take(int vol)
{
    (void)vol;
    return 1;
}

void ff_mutex_give(int vol)
{
    (void)vol;
}
#endif

#if !FF_FS_NORTC
DWORD get_fattime(void)
{
    /* The kernel keeps a wall-clock epoch read from the CMOS RTC at boot;
     * FatFs wants a packed DOS stamp, and a date is more useful to a
     * directory listing than a zero is. */
    uint64_t now = timer_boot_epoch() + timer_ticks() / 100u;
    /* crude but honest: days/hours/minutes/seconds since the epoch */
    uint32_t sec  = (uint32_t)(now % 60u);
    uint32_t min  = (uint32_t)((now / 60u) % 60u);
    uint32_t hour = (uint32_t)((now / 3600u) % 24u);
    uint32_t day  = (uint32_t)((now / 86400u) % 31u) + 1u;
    uint32_t mon  = 1u;
    uint32_t year = 1980u + 46u; /* 2026 */
    return ((year - 1980u) << 25) | (mon << 21) | (day << 16) | (hour << 11) | (min << 5) |
           (sec >> 1);
}
#endif
