/* SPDX-License-Identifier: GPL-2.0 */
/*
 * syscall_impl.h — the contract between the dispatch table and its handlers.
 * (GPLv2)
 *
 * Linux keeps its sys_call_table[] and the prototypes of every __x64_sys_*
 * entry point in sync by including one list twice with two different macro
 * definitions.  We do the same: syscall_list.h is included here (pass 1, to
 * declare the handlers) and again in dispatch.c (pass 2, to fill the table).
 *
 * Two families of functions live in syscall.c / sysuser.c:
 *   sys_*        typed implementations, file-local, unchanged signatures
 *   __x64_sys_*  uniform ABI entry points taking the saved register frame;
 *                these are the ones the dispatch table points at.
 */
#ifndef GNUCOS_SYSCALL_IMPL_H
#define GNUCOS_SYSCALL_IMPL_H

#include <stdint.h>
#include "panic.h"  /* regs_t */
#include "sysnum.h" /* SYS_* */

/* Linux x86-64 passes the whole pt_regs frame to every table entry; regs_t
 * is our equivalent.  A single signature keeps the table a plain array. */
typedef int64_t (*sys_call_ptr_t)(regs_t *r);

/* execve() on success and rt_sigreturn() replace the entire register frame:
 * the dispatcher must not write RAX, record the syscall in the history ring
 * or deliver a ptrace syscall-exit stop afterwards.  Those two handlers
 * return this sentinel instead of a result.  No real syscall can produce it
 * (errors are small negative errnos; successes are counts, pids or pointers).
 * INT64_MIN is not defined by our freestanding <stdint.h>, so spell it out. */
#define SYS_FRAME_REPLACED (-INT64_MAX - 1LL)

/* Pass 1: one prototype per entry point. */
#define __SYSCALL(nr, fn) int64_t fn(regs_t *r);
#include "syscall_list.h"
#undef __SYSCALL

/* Path/user-buffer helpers are defined once in syscall.c but the credential
 * and permission implementations in sysuser.c need them too. */
int buf_ok(uint64_t p, uint64_t len, int writing);
int path_abs(uint64_t upath, char *out);
int path_at(int dfd, uint64_t upath, char *out);

#endif
