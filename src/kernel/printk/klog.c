/* SPDX-License-Identifier: GPL-2.0 */
/*
 * klog.c — the kernel log ring: every debugcon line is recorded here and
 * readable from user space (dmesg-style). (GPLv2)
 *
 * dbg_puts()/dbg_puts_dec()/dbg_puts_hex() feed the ring as well as the
 * debug port, so a headless boot is fully reconstructable from inside the
 * guest.  sys_klog() exposes the classic syslog(2)-style ops:
 *
 *   klog(0)              read pending bytes (drains the unread tail)
 *   klog(2)              read without draining
 *   klog(3)              clear the unread tail
 *   klog(6, buf, len)    clear the whole ring
 *   klog(9|10, buf, len) write a line into the ring from user space
 */
#include <stdint.h>
#include <stdarg.h>

#include "klog.h"
#include "kstring.h"
#include "sysnum.h"
#include "vmm.h"
#include "vfs.h"
#include "heap.h"

#define KLOG_SIZE  (256 * 1024)      /* power of two */

static char    *klog_buf;
static uint32_t klog_head;            /* next write offset */
static uint32_t klog_tail;            /* oldest unread byte (drains on read) */
static uint32_t klog_written;         /* total bytes ever written */

void klog_append(const char *s, uint32_t len)
{
    if (!klog_buf) {
        klog_buf = kmalloc(KLOG_SIZE);
        if (!klog_buf)
            return;
        memset(klog_buf, 0, KLOG_SIZE);
    }
    for (uint32_t i = 0; i < len; i++) {
        klog_buf[klog_head] = s[i];
        klog_head = (klog_head + 1) % KLOG_SIZE;
        if (klog_head == klog_tail)          /* full ring: push the tail */
            klog_tail = (klog_tail + 1) % KLOG_SIZE;
        klog_written++;
    }
}

/* The debugcon feeders call in here so the ring mirrors the port exactly. */
void klog_dbg_line(const char *s)
{
    klog_append(s, (uint32_t)strlen(s));
}

void klog_init(void)
{
    if (!klog_buf) {
        klog_buf = kmalloc(KLOG_SIZE);
        if (klog_buf)
            memset(klog_buf, 0, KLOG_SIZE);
    }
}

/* ---- user interface (sys_klog) ------------------------------------------ */

int64_t klog_syscall(uint64_t op, uint64_t buf, uint64_t len)
{
    if (!klog_buf)
        klog_init();
    if (!klog_buf)
        return -E_NOMEM;

    switch (op) {
    case 0:                                     /* read + drain */
    case 2: {                                   /* read, do not drain */
        uint64_t avail = (klog_head - klog_tail + KLOG_SIZE) % KLOG_SIZE;
        uint64_t n = (len < avail) ? len : avail;
        if (!n)
            return 0;
        if (!user_ptr_ok(buf, n))
            return -E_FAULT;
        uint64_t first = KLOG_SIZE - klog_tail;
        if (first > n)
            first = n;
        memcpy((void *)(uintptr_t)buf, klog_buf + klog_tail, first);
        if (n > first)
            memcpy((void *)(uintptr_t)(buf + first), klog_buf, n - first);
        if (op == 0)
            klog_tail = (klog_tail + (uint32_t)n) % KLOG_SIZE;
        return (int64_t)n;
    }
    case 3:                                     /* clear the unread tail */
        klog_tail = klog_head;
        return 0;
    case 6: {                                   /* clear the whole ring */
        klog_head = klog_tail = 0;
        klog_written = 0;
        return 0;
    }
    case 9:
    case 10: {                                  /* write from user space */
        if (!len)
            return 0;
        if (len > 4096)
            len = 4096;
        if (!user_ptr_ok(buf, len))
            return -E_FAULT;
        klog_append((const char *)(uintptr_t)buf, (uint32_t)len);
        klog_append("\n", 1);
        return 0;
    }
    default:
        return -E_INVAL;
    }
    (void)klog_written;
}
