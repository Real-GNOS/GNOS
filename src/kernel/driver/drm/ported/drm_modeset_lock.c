/*
 * drm_modeset_lock.c - locking for display-configuration changes. (GPLv2)
 *
 * See drm_modeset_lock.h for why acquisition can fail instead of waiting.
 *
 * Lock order, which is what keeps this deadlock-free at all: a context's
 * bookkeeping lock is the innermost lock in the system.  It is never held
 * while taking an object lock, and never held across a call that takes it
 * again.  Object locks are taken one at a time and failures unwind instead
 * of waiting, so nothing ever waits for a lock while holding another.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_modeset_lock.h"
#include "intrusive_list.h"
#include "smp.h"
#include "vfs.h"

/* The object @node belongs to, given that @node is its ->link member. */
#define lock_of(node) \
    ((struct drm_modeset_lock *)((uintptr_t)(node) - offsetof(struct drm_modeset_lock, link)))

/* ------------------------------------------------------------- single lock */

void drm_modeset_lock_init(struct drm_modeset_lock *lock)
{
    lock->mutex.v = 0;
    lock->ctx     = NULL;

    /* An empty ring: removing this link later is then always safe, whether
     * or not the lock was ever handed to a context. */
    ilist_init(&lock->link);
}

int drm_modeset_lock(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx)
{
    if (ctx == NULL) {
        /* Untracked: no context to consult or record. */
        spin_lock(&lock->mutex);
        lock->ctx = NULL;
        return 0;
    }

    if (lock->ctx == ctx) { return 0; } /* taking it twice is free */

    if (lock->ctx != NULL) {
        /* Somebody else owns it.  Note which lock that was so backoff can
         * take it first next time, and refuse rather than wait. */
        spin_lock(&ctx->ctx_lock);
        ctx->contended_lock = lock;
        spin_unlock(&ctx->ctx_lock);
        return -EDEADLK;
    }

    spin_lock(&lock->mutex);

    /* Holding the mutex means the previous owner has finished releasing,
     * so no context can be recorded for this lock any more. */
    spin_lock(&ctx->ctx_lock);
    lock->ctx = ctx;
    ilist_insert_after(&ctx->locked, &lock->link);
    ctx->num_locks++;
    spin_unlock(&ctx->ctx_lock);

    return 0;
}

int drm_modeset_lock_interruptible(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx)
{
    /* There is nothing here that could be interrupted yet. */
    return drm_modeset_lock(lock, ctx);
}

void drm_modeset_unlock(struct drm_modeset_lock *lock)
{
    struct drm_modeset_acquire_ctx *ctx = lock->ctx;

    if (ctx != NULL) {
        /* Clear ownership first: whoever takes the mutex next reads ->ctx
         * to decide whether it is free. */
        spin_lock(&ctx->ctx_lock);
        lock->ctx = NULL;
        ilist_remove(&lock->link);
        spin_unlock(&ctx->ctx_lock);
    }

    spin_unlock(&lock->mutex);
}

int drm_modeset_lock_single_interruptible(struct drm_modeset_lock *lock)
{
    unsigned previous = __atomic_exchange_n(&lock->mutex.v, 1u, __ATOMIC_ACQUIRE);

    if (previous != 0) { return -EBUSY; }

    lock->ctx = NULL;
    return 0;
}

bool drm_modeset_is_locked(struct drm_modeset_lock *lock)
{
    return lock->ctx != NULL || lock->mutex.v != 0;
}

/* ------------------------------------------------------------ a transaction */

void drm_modeset_acquire_init(struct drm_modeset_acquire_ctx *ctx, uint32_t flags)
{
    ctx->ctx_lock.v     = 0;
    ilist_init(&ctx->locked);
    ctx->contended_lock = NULL;
    ctx->trylock_only   = false;
    ctx->interruptible  = (flags & 0x1U) != 0;
    ctx->num_locks      = 0;
}

void drm_modeset_acquire_fini(struct drm_modeset_acquire_ctx *ctx)
{
    drm_modeset_drop_locks(ctx);
}

int drm_modeset_drop_locks(struct drm_modeset_acquire_ctx *ctx)
{
    ilist_node_t *node = ctx->locked.prev;

    while (node != &ctx->locked) {
        /* Grab the next victim first: unlocking unlinks this one. */
        ilist_node_t *earlier = node->prev;

        drm_modeset_unlock(lock_of(node));
        node = earlier;
    }

    spin_lock(&ctx->ctx_lock);
    ctx->num_locks = 0;
    spin_unlock(&ctx->ctx_lock);

    return 0;
}

int drm_modeset_backoff(struct drm_modeset_acquire_ctx *ctx)
{
    struct drm_modeset_lock *contended = ctx->contended_lock;

    if (contended == NULL) { return 0; }

    /* Everything goes back.  contended_lock survives this call, which is
     * the whole point. */
    drm_modeset_drop_locks(ctx);

    /* Now wait for whichever context had it: once its owner releases, ->ctx
     * is NULL and the lock is ours to claim. */
    spin_lock(&contended->mutex);

    spin_lock(&ctx->ctx_lock);
    contended->ctx = ctx;
    ilist_insert_after(&ctx->locked, &contended->link);
    ctx->num_locks++;
    ctx->contended_lock = NULL;
    spin_unlock(&ctx->ctx_lock);

    return 0;
}

/* -------------------------------------------------------- whole-device lock */

int drm_modeset_lock_all_ctx(struct drm_device *dev, struct drm_modeset_acquire_ctx *ctx)
{
    ilist_node_t *node;

    /* The mode-config lock is a plain spinlock here, so it sits outside the
     * context's bookkeeping and has to be released by the caller. */
    spin_lock(&dev->mode_config.mutex);

    for (node = dev->mode_config.crtc_list.next; node != &dev->mode_config.crtc_list; node = node->next) {
        struct drm_crtc *crtc = (struct drm_crtc *)((uintptr_t)node - offsetof(struct drm_crtc, head));
        int              ret  = drm_modeset_lock(&crtc->mutex, ctx);

        if (ret != 0) {
            /* Give back what we took so the retry cannot deadlock against
             * our own earlier self. */
            spin_unlock(&dev->mode_config.mutex);
            return ret;
        }
    }

    return 0;
}
