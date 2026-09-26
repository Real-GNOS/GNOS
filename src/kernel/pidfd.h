/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pidfd.h — pidfd: a process referred to by a descriptor. (GPLv2)
 *
 * Linux added pidfd (5.3) because a pid is a number that can be recycled:
 * between the moment a supervisor reads a pid and the moment it signals it,
 * the process can exit and the number can be handed to someone else.  A
 * descriptor holds a reference to the process itself, so it cannot be
 * confused that way.
 */
#ifndef GNUCOS_PIDFD_H
#define GNUCOS_PIDFD_H

#include <stdint.h>

struct proc;
struct proc *pidfd_proc_of(int fd);      /* NULL if fd is not a pidfd */

int64_t sys_pidfd_open(uint64_t pid, uint64_t flags);
int64_t sys_pidfd_send_signal(uint64_t pidfd, uint64_t sig, uint64_t uinfo,
                              uint64_t flags);
int64_t sys_pidfd_getfd(uint64_t pidfd, uint64_t targetfd, uint64_t flags);

#endif
