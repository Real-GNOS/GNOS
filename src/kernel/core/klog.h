/*
 * klog.h — the kernel log ring (dmesg-style), readable from user space.
 * (GPLv2)
 */
#ifndef GNUCOS_KLOG_H
#define GNUCOS_KLOG_H

#include <stdint.h>

/* Append raw bytes into the ring (called from debugcon.c). */
void klog_append(const char *s, uint32_t len);
void klog_dbg_line(const char *s);

/* Allocate the ring early (before the first dbg output). */
void klog_init(void);

/* sys_klog(443): op 0 read+drain, 2 read, 3 clear tail, 6 clear all,
 * 9/10 write a line from user space. */
int64_t klog_syscall(uint64_t op, uint64_t buf, uint64_t len);

#endif
