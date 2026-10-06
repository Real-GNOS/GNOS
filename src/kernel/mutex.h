/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mutex.h — the kernel's sleeping mutex. (GPLv2)
 *
 * A spinlock burns the competing core; a mutex parks the loser on a
 * wait queue and wakes it on release.  Which one a section wants
 * depends on the length of the section: anything that can sleep, take
 * a timer tick, or touch a structure the scheduler also walks must not
 * hold a spinlock, and that is exactly what this is for.
 */
#ifndef GNUCOS_MUTEX_H
#define GNUCOS_MUTEX_H

#include <stdint.h>
#include "smp.h" /* spinlock_t guards the mutex's own fields */

struct proc;

typedef struct mutex {
    spinlock_t   slot;   /* guards owner/waiters/count below */
    struct proc *owner;  /* current holder, NULL when free */
    struct proc *waiters; /* FIFO of parked processes */
    uint32_t     count;  /* wait queue length, for the fast check */
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_destroy(mutex_t *m);

/* Acquire.  Parks the caller on m's FIFO when contended, with the wait
 * reason WAIT_MUTEX, and returns holding the mutex. */
void mutex_lock(mutex_t *m);

/* Try once.  Returns 1 on success (now holding), 0 when contended. */
int mutex_trylock(mutex_t *m);

/* Release and wake the head waiter, if any. */
void mutex_unlock(mutex_t *m);

/* True when the calling process holds m (for assertions/diagnostics). */
int mutex_held(mutex_t *m);

#endif
