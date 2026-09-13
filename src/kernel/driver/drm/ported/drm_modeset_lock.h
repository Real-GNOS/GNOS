/*
 * drm_modeset_lock.h - locking for display-configuration changes. (GPLv2)
 *
 * Changing a display configuration means touching a CRTC, its plane, an
 * encoder and a connector (and possibly several of each), so complications
 * live entirely in *taking many locks at once*.  Two threads doing that in
 * opposite orders deadlock, which is why acquisition here is never allowed
 * to block quietly: an acquire context records what it already holds, and
 * when the next lock turns out to be owned elsewhere the caller gets
 * -EDEADLK, drops everything and tries again with the contended lock first.
 *
 * A context belongs to one thread for the duration of a transaction; the
 * locks themselves are ordinary spinlocks held from acquisition to release.
 */

#ifndef INCLUDE_DRM_DRM_MODESET_LOCK_H_
#define INCLUDE_DRM_DRM_MODESET_LOCK_H_

#include <stdbool.h>
#include <stdint.h>

#include "intrusive_list.h"
#include "smp.h"

struct drm_device;

/* One lock protecting one display object. */
struct drm_modeset_lock {
    spinlock_t                      mutex; /* held for the whole ownership window */
    struct drm_modeset_acquire_ctx *ctx;   /* owner, NULL when unowned */
    ilist_node_t                    link;  /* membership in ctx->locked */
};

/* Ready-made for a statically allocated lock. */
#define DRM_MODESET_LOCK_INIT(lock) \
    do {                            \
        (lock)->mutex.v = 0;        \
        (lock)->ctx     = NULL;     \
    } while (0)

/* What one thread knows while it is taking locks. */
struct drm_modeset_acquire_ctx {
    spinlock_t               ctx_lock;       /* guards the fields below    */
    struct drm_modeset_lock *contended_lock; /* what the last failure hit  */
    ilist_node_t             locked;         /* every lock this ctx holds  */
    bool                     trylock_only;
    bool                     interruptible;
    int                      num_locks;
};

/* Prepare @lock for use, leaving it unowned. */
void drm_modeset_lock_init(struct drm_modeset_lock *lock);

/*
 * Take @lock for @ctx.  Returns 0, or -EDEADLK when a different context
 * already owns it (the caller is expected to back off and retry).
 * Passing @ctx == NULL takes the lock untracked, with no deadlock handling.
 */
int drm_modeset_lock(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx);

/* Same, spelled the way callers that could sleep will later spell it. */
int drm_modeset_lock_interruptible(struct drm_modeset_lock *lock, struct drm_modeset_acquire_ctx *ctx);

/* Release @lock and forget its owner. */
void drm_modeset_unlock(struct drm_modeset_lock *lock);

/*
 * Take @lock on its own, without an acquire context, and without waiting:
 * returns 0, or -EBUSY when somebody else holds it.
 */
int drm_modeset_lock_single_interruptible(struct drm_modeset_lock *lock);

/* True when @lock is owned right now, tracked or not. */
bool drm_modeset_is_locked(struct drm_modeset_lock *lock);

/* Prepare @ctx to start collecting locks.  Bit 0 of @flags asks for
 * interruptible acquisition; @flags is otherwise reserved. */
void drm_modeset_acquire_init(struct drm_modeset_acquire_ctx *ctx, uint32_t flags);

/* End a transaction, releasing whatever is still held. */
void drm_modeset_acquire_fini(struct drm_modeset_acquire_ctx *ctx);

/* Let go of every lock @ctx holds, newest first. */
int drm_modeset_drop_locks(struct drm_modeset_acquire_ctx *ctx);

/* Recover from a -EDEADLK: drop everything, then take the lock that blocked
 * us, so the caller's retry starts already owning it. */
int drm_modeset_backoff(struct drm_modeset_acquire_ctx *ctx);

/*
 * Take every modeset lock a device has: the mode-config lock plus one per
 * CRTC.  On failure everything already taken is released so the caller's
 * retry starts from clean ground.
 */
int drm_modeset_lock_all_ctx(struct drm_device *dev, struct drm_modeset_acquire_ctx *ctx);

#endif /* INCLUDE_DRM_DRM_MODESET_LOCK_H_ */
