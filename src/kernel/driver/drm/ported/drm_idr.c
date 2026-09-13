/*
 * drm_idr.c ¡ª id -> pointer table behind the DRM object handles. (GPLv2)
 *
 * The table is a plain open-addressed hash map over power-of-two buckets
 * with linear probing.  That choice is deliberate: the whole contents of a
 * table like this at runtime is a few dozen entries (the CRTCs, connectors
 * and friends a machine actually has, or the handful of GEM handles one fd
 * holds), so the goal is not asymptotic elegance but small code, predictable
 * latency and no allocation during lookup.
 *
 * Removal is what makes an open-addressed table interesting: a probe walks
 * a chain of buckets and stops at the first bucket that was never used, so
 * simply blanking a bucket would cut every chain that ran through it and
 * make entries behind it unreachable.  Removed buckets are therefore marked
 * DEAD -- dead slabs are skipped by lookups, reused by insertions, and
 * dropped wholesale whenever the table doubles.
 */

#include "drm_port.h"
#include "drm_idr.h"

/* First bucket count.  Small on purpose: the tables start nearly empty. */
#define DRM_IDR_MIN_CAPACITY 64u

/* Double the table once this fraction of its buckets is live. */
#define DRM_IDR_LOAD_NUMER 3u
#define DRM_IDR_LOAD_DENOM 4u

/* Scramble the low bits of an id into a bucket index.  ids are allocated
 * mostly sequentially, so without this every insertion would pile onto the
 * next bucket along and every lookup would walk the whole pile.  The
 * multiplier is the golden-ratio 32-bit constant (2654435761). */
static inline uint32_t drm_idr_bucket(uint32_t id, uint32_t capacity)
{
    return (id * 2654435761u) & (capacity - 1u);
}

/* True when a bucket holds nothing that could belong to a probe. */
static inline bool drm_idr_slot_unused(const struct drm_idr_entry *e)
{
    return e->state == DRM_IDR_SLOT_EMPTY || e->state == DRM_IDR_SLOT_DEAD;
}

/*
 * Double @idr's buckets and rehash the live entries.  Callers hold the lock.
 * Returns 0, or -ENOMEM when the kernel heap cannot supply the new array.
 */
static int drm_idr_rehash(struct drm_idr *idr)
{
    struct drm_idr_entry *fresh;
    uint32_t              capacity = idr->capacity * 2u;
    uint32_t              i;

    if (capacity == 0u || capacity < idr->capacity) { return -ENOMEM; }

    fresh = malloc(capacity * sizeof(struct drm_idr_entry));
    if (fresh == NULL) { return -ENOMEM; }
    memset(fresh, 0, capacity * sizeof(struct drm_idr_entry));

    for (i = 0; i < idr->capacity; i++) {
        struct drm_idr_entry *src = &idr->table[i];
        uint32_t              idx;

        if (src->state != DRM_IDR_SLOT_LIVE) { continue; }

        idx = drm_idr_bucket(src->id, capacity);
        while (fresh[idx].state == DRM_IDR_SLOT_LIVE) {
            idx = (idx + 1u) & (capacity - 1u);
        }
        fresh[idx] = *src;
    }

    free(idr->table);
    idr->table    = fresh;
    idr->capacity = capacity;
    return 0;
}

/* Make room for one more entry when the table is getting tight. */
static int drm_idr_reserve(struct drm_idr *idr)
{
    uint64_t used = (uint64_t)idr->count + 1u;

    if (used * DRM_IDR_LOAD_DENOM <= (uint64_t)idr->capacity * DRM_IDR_LOAD_NUMER) {
        return 0;
    }
    return drm_idr_rehash(idr);
}

/*
 * Find the live entry for @id.  Callers hold the lock.
 * Returns NULL when @id is not in the table.
 */
static struct drm_idr_entry *drm_idr_locate(struct drm_idr *idr, uint32_t id)
{
    uint32_t mask = idr->capacity - 1u;
    uint32_t idx  = drm_idr_bucket(id, idr->capacity);
    uint32_t i;

    if (idr->capacity == 0u) { return NULL; }

    for (i = 0; i < idr->capacity; i++) {
        struct drm_idr_entry *e = &idr->table[idx];

        /* An unused bucket that has never been used ends the probe chain
         * -- nothing can have been placed past it. */
        if (e->state == DRM_IDR_SLOT_EMPTY) { return NULL; }
        if (e->state == DRM_IDR_SLOT_LIVE && e->id == id) { return e; }
        idx = (idx + 1u) & mask;
    }
    return NULL;
}

/*
 * Bind @id to @ptr.  Callers hold the lock.  Returns 0, -EEXIST when @id is
 * already bound, or -ENOMEM when the table cannot grow to make room.
 */
static int drm_idr_bind(struct drm_idr *idr, void *ptr, uint32_t id)
{
    int grown = 0;

    for (;;) {
        uint32_t              mask = idr->capacity - 1u;
        uint32_t              idx  = drm_idr_bucket(id, idr->capacity);
        struct drm_idr_entry *dead = NULL;
        uint32_t              i;

        if (idr->capacity == 0u) {
            if (drm_idr_rehash(idr) != 0) { return -ENOMEM; }
            continue;
        }

        for (i = 0; i < idr->capacity; i++) {
            struct drm_idr_entry *e = &idr->table[idx];

            if (e->state == DRM_IDR_SLOT_EMPTY) {
                /* Free spot: reuse the tombstone we walked past, if any, so
                 * dead buckets do not silently accumulate. */
                e         = (dead != NULL) ? dead : e;
                e->id     = id;
                e->ptr    = ptr;
                e->state  = DRM_IDR_SLOT_LIVE;
                idr->count++;
                return 0;
            }
            if (e->state == DRM_IDR_SLOT_LIVE) {
                if (e->id == id) { return -EEXIST; }
            } else if (dead == NULL) {
                dead = e;
            }
            idx = (idx + 1u) & mask;
        }

        /* Every bucket is live: grow and try once more in the new table. */
        if (grown || drm_idr_rehash(idr) != 0) { return -ENOMEM; }
        grown = 1;
    }
}

void drm_idr_init(struct drm_idr *idr)
{
    memset(idr, 0, sizeof(*idr));

    idr->table = malloc(DRM_IDR_MIN_CAPACITY * sizeof(struct drm_idr_entry));
    if (idr->table == NULL) { return; }
    memset(idr->table, 0, DRM_IDR_MIN_CAPACITY * sizeof(struct drm_idr_entry));

    idr->capacity = DRM_IDR_MIN_CAPACITY;
    idr->next_id  = 1u;
}

void drm_idr_destroy(struct drm_idr *idr)
{
    spin_lock(&idr->lock);

    free(idr->table);
    idr->table    = NULL;
    idr->capacity = 0;
    idr->count    = 0;
    idr->next_id  = 0;

    spin_unlock(&idr->lock);
}

int drm_idr_alloc(struct drm_idr *idr, void *ptr, uint32_t start, uint32_t end, uint32_t *id_out)
{
    uint32_t last = (end != 0u) ? (end - 1u) : (UINT32_MAX - 1u);
    uint32_t id;
    int      ret;

    spin_lock(&idr->lock);

    if (drm_idr_reserve(idr) != 0) {
        spin_unlock(&idr->lock);
        return -ENOMEM;
    }

    id = (start > idr->next_id) ? start : idr->next_id;
    for (; id <= last; id++) {
        ret = drm_idr_bind(idr, ptr, id);
        if (ret == 0) {
            /* Keep the hint monotonic so repeated allocations do not hand
             * out an id that was used once, freed, and is being watched by
             * some user of the old handle. */
            if (idr->next_id <= id) { idr->next_id = id + 1u; }
            *id_out = id;
            spin_unlock(&idr->lock);
            return 0;
        }
        if (ret != -EEXIST) {
            spin_unlock(&idr->lock);
            return ret;
        }
    }

    spin_unlock(&idr->lock);
    return -ENOSPC;
}

int drm_idr_alloc_exact(struct drm_idr *idr, void *ptr, uint32_t id)
{
    int ret;

    if (id == DRM_IDR_INVALID) { return -EINVAL; }

    spin_lock(&idr->lock);

    if (drm_idr_reserve(idr) != 0) {
        spin_unlock(&idr->lock);
        return -ENOMEM;
    }

    ret = drm_idr_bind(idr, ptr, id);

    spin_unlock(&idr->lock);
    return ret;
}

void *drm_idr_find(struct drm_idr *idr, uint32_t id)
{
    struct drm_idr_entry *e;
    void                 *ptr = NULL;

    if (id == DRM_IDR_INVALID) { return NULL; }

    spin_lock(&idr->lock);
    e = drm_idr_locate(idr, id);
    if (e != NULL) { ptr = e->ptr; }
    spin_unlock(&idr->lock);

    return ptr;
}

void *drm_idr_remove(struct drm_idr *idr, uint32_t id)
{
    struct drm_idr_entry *e;
    void                 *ptr = NULL;

    if (id == DRM_IDR_INVALID) { return NULL; }

    spin_lock(&idr->lock);
    e = drm_idr_locate(idr, id);
    if (e != NULL) {
        ptr        = e->ptr;
        e->ptr     = NULL;
        e->state   = DRM_IDR_SLOT_DEAD;
        idr->count--;
    }
    spin_unlock(&idr->lock);

    return ptr;
}

void *drm_idr_replace(struct drm_idr *idr, void *ptr, uint32_t id)
{
    struct drm_idr_entry *e;
    void                 *old = NULL;

    if (id == DRM_IDR_INVALID) { return NULL; }

    spin_lock(&idr->lock);
    e = drm_idr_locate(idr, id);
    if (e != NULL) {
        old    = e->ptr;
        e->ptr = ptr;
    }
    spin_unlock(&idr->lock);

    return old;
}

int drm_idr_for_each(struct drm_idr *idr, int (*fn)(uint32_t id, void *ptr, void *data), void *data)
{
    uint32_t i;
    int      ret = 0;

    spin_lock(&idr->lock);
    for (i = 0; i < idr->capacity; i++) {
        struct drm_idr_entry *e = &idr->table[i];

        if (e->state != DRM_IDR_SLOT_LIVE) { continue; }
        ret = fn(e->id, e->ptr, data);
        if (ret != 0) { break; }
    }
    spin_unlock(&idr->lock);

    return ret;
}
