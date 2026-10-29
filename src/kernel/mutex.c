/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mutex.c — the kernel's sleeping mutex. (GPLv2)
 *
 * The fast path is one atomic compare-and-swap on owner; the slow path
 * parks the caller on the mutex's FIFO wait queue (wait reason
 * WAIT_MUTEX) and the releaser wakes the head.  FIFO ordering is the
 * point: it bounds how long any one waiter can starve, which a bare
 * spin loop does not.
 *
 * The mutex's own fields are guarded by slot, a spinlock held only for
 * the few instructions it takes to link or unlink a waiter -- never
 * across a sleeping section, so it stays honest as a spinlock.
 *
 * Blocking goes through sched_block(WAIT_MUTEX) with IRQs already off
 * (sched_block saves and restores IF itself; we drop slot before
 * blocking so an interrupt arriving between can never find a waiter
 * linked but not yet parked).
 */
#include <stddef.h>
#include <stdint.h>
#include "mutex.h"
#include "proc.h"
#include "smp.h"

void mutex_init(mutex_t *m)
{
    m->slot.v           = 0;
    m->slot.saved_flags = 0;
    m->owner            = NULL;
    m->waiters          = NULL;
    m->count            = 0;
}

void mutex_destroy(mutex_t *m)
{
    /* Destroying a held or waited-on mutex is a kernel bug; leave the
     * fields intact so a post-mortem can see it. */
    if (m->owner || m->waiters)
        return;
    m->slot.v = 0;
}

int mutex_trylock(mutex_t *m)
{
    spin_lock_irq(&m->slot);
    if (m->owner) {
        spin_unlock_irq(&m->slot);
        return 0;
    }
    m->owner = proc_current();
    spin_unlock_irq(&m->slot);
    return 1;
}

void mutex_lock(mutex_t *m)
{
    spin_lock_irq(&m->slot);

    if (!m->owner) {
        m->owner = proc_current();
        spin_unlock_irq(&m->slot);
        return;
    }

    /* Contended: link onto the FIFO, then park.  slot must be released
     * before blocking -- the releaser needs it to reach this waiter. */
    proc_t *cur     = proc_current();
    cur->mutex_next = NULL;
    cur->wait_mutex = (struct mutex *)m;
    if (m->waiters) {
        struct proc *t = m->waiters;
        while (t->mutex_next)
            t = t->mutex_next;
        t->mutex_next = cur;
    } else {
        m->waiters = cur;
    }
    m->count++;

    spin_unlock_irq(&m->slot);
    sched_block(WAIT_MUTEX);

    /* Back from the scheduler: the releaser handed ownership to us
     * (wait_mutex cleared, owner set) before waking us. */
    cur->mutex_next = NULL;
}

void mutex_unlock(mutex_t *m)
{
    spin_lock_irq(&m->slot);

    if (m->owner != proc_current()) {
        /* Releasing somebody else's mutex is a kernel bug; refuse rather
         * than hand ownership to a stranger. */
        spin_unlock_irq(&m->slot);
        return;
    }

    struct proc *next = m->waiters;
    if (next) {
        m->waiters = next->mutex_next;
        m->count--;
        next->mutex_next = NULL;
        next->wait_mutex = NULL;
        m->owner         = next;
        spin_unlock_irq(&m->slot);
        sched_wake(next);
        return;
    }

    m->owner = NULL;
    spin_unlock_irq(&m->slot);
}

int mutex_held(mutex_t *m)
{
    return m->owner == proc_current();
}
