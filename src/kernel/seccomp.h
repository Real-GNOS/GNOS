/*
 * seccomp.h — seccomp-BPF filtering. (GPLv2)
 */
#ifndef GNUOS_SECCOMP_H
#define GNUOS_SECCOMP_H

#include "proc.h"

/* Evaluate a seccomp filter against the current syscall.
 * Returns 0 to allow, negative to deny/kill.  Called from syscall_handler()
 * with the BKL held. */
int seccomp_check(proc_t *p, int syscall_nr, uint64_t a1, uint64_t a2,
                  uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

/* Handle prctl() calls related to seccomp (PR_SET/GET_SECCOMP,
 * PR_SET/GET_NO_NEW_PRIVS). */
int seccomp_prctl(proc_t *p, int option, uint64_t arg2);

/* Handle the seccomp(2) syscall. */
int sys_seccomp(uint64_t op, uint64_t flags, uint64_t u_fprog);

/* Called on exec: filter survives exec (no-op for now). */
void seccomp_exec(proc_t *p);

/* Called on exit: free the BPF filter. */
void seccomp_free(proc_t *p);

#endif
