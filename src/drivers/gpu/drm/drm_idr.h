/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_idr.h — small integer ID allocator used by the DRM core. (GPLv2)
 *
 * Every DRM mode object (CRTC, connector, encoder, plane, framebuffer,
 * property) and every GEM handle a userspace fd owns is reached by a 32-bit
 * id rather than a pointer: user space never learns kernel addresses, and
 * an id handed to a dead or foreign file descriptor simply fails to look
 * up instead of being dereferenced.
 *
 * The API mirrors the subset of Linux's IDR that the DRM port relies on.
 * id 0 is never handed out, so it can mean "no object".
 */

#ifndef INCLUDE_DRM_DRM_IDR_H_
#define INCLUDE_DRM_DRM_IDR_H_

#include <stddef.h>
#include <stdint.h>

#include "smp.h"

/* One slot of the hash table.  `state` distinguishes the three things a
 * slot can hold, because an open-addressed table cannot tell "never used"
 * from "used and then removed" by looking at the payload alone. */
enum drm_idr_slot_state {
    DRM_IDR_SLOT_EMPTY = 0, /* never held an entry; a probe stops here  */
    DRM_IDR_SLOT_LIVE,      /* holds id/ptr                             */
    DRM_IDR_SLOT_DEAD       /* held an entry that was removed           */
};

struct drm_idr_entry {
    uint32_t                 id;
    void                    *ptr;
    enum drm_idr_slot_state  state;
};

struct drm_idr {
    spinlock_t            lock;     /* covers every member below          */
    uint32_t              next_id;  /* first id to try on the next alloc   */
    struct drm_idr_entry *table;    /* open-addressed buckets              */
    uint32_t              capacity; /* always a power of two               */
    uint32_t              count;    /* live entries (tombstones excluded)  */
};

#define DRM_IDR_INVALID 0U

/* Prepare @idr for use.  Allocates its first bucket array; if that fails
 * the idr is left empty and every later call reports -ENOMEM or NULL. */
void drm_idr_init(struct drm_idr *idr);

/* Release @idr's storage.  The payload pointers are owned by the caller
 * and are *not* freed. */
void drm_idr_destroy(struct drm_idr *idr);

/*
 * Bind @ptr to a fresh id and report it in *@id_out.
 * The id is chosen in [start, end); @end == 0 means "up to UINT32_MAX".
 * Returns 0, or -ENOMEM / -ENOSPC when no id is available.
 */
int drm_idr_alloc(struct drm_idr *idr, void *ptr, uint32_t start, uint32_t end, uint32_t *id_out);

/* Bind @ptr to exactly @id.  Returns 0, -EINVAL for id 0, -EEXIST if @id
 * is taken, or -ENOMEM / -ENOSPC on resource exhaustion. */
int drm_idr_alloc_exact(struct drm_idr *idr, void *ptr, uint32_t id);

/* Return what @id is bound to, or NULL when @id is free/invalid. */
void *drm_idr_find(struct drm_idr *idr, uint32_t id);

/* Unbind @id and return what it was bound to (NULL when free/invalid). */
void *drm_idr_remove(struct drm_idr *idr, uint32_t id);

/* Rebind an existing @id to @ptr and return the previous pointer. */
void *drm_idr_replace(struct drm_idr *idr, void *ptr, uint32_t id);

/*
 * Walk every live entry, in table order (not id order).
 * @fn returning non-zero stops the walk, and that value comes back here.
 */
int drm_idr_for_each(struct drm_idr *idr, int (*fn)(uint32_t id, void *ptr, void *data), void *data);

#endif /* INCLUDE_DRM_DRM_IDR_H_ */
