/* SPDX-License-Identifier: GPL-2.0 */
/*
 * inotify.h — the inotify syscall layer. (GPLv2)
 */
#ifndef GNUCOS_INOTIFY_H
#define GNUCOS_INOTIFY_H

#include <stdint.h>

/* The Linux UAPI event bits, shared between the implementation and the
 * VFS hooks that fire them. */
#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ISDIR         0x40000000

#define IN_CLOEXEC  0x00080000
#define IN_NONBLOCK 0x00008000

int64_t sys_inotify_init1(uint64_t flags);
int64_t sys_inotify_init(void);
int64_t sys_inotify_add_watch(int fd, uint64_t upath, uint32_t mask);
int64_t sys_inotify_rm_watch(int fd, uint32_t wd);

/* Called by the VFS mutation hooks: report one event against `path`.
 * `mask` is an IN_* event bit (IN_ISDIR included where known); `cookie`
 * correlates a MOVED_FROM/MOVED_TO rename pair, 0 elsewhere.  Defined in
 * inotify.c. */
void inotify_event_fire(const char *path, uint32_t mask, uint32_t cookie);

#endif
