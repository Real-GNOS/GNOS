/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_gem.c - buffer objects: who owns the pixels. (GPLv2)
 *
 * A GEM object is a reference-counted block of memory with two ways of
 * being named.  A handle is a small integer meaningful only inside one
 * open file, and is what a normal client uses; a flink name is global, and
 * exists so two processes can find the same buffer.  Both are indirections
 * over the same object, which is what lets the kernel keep the memory
 * alive as long as anybody refers to it and free it the moment nobody
 * does.
 *
 * The mmap-offset allocator at the top is the piece that makes dumb
 * buffers work: when a client mmaps a buffer it gives us an offset, and we
 * have to turn that back into an object.  Offsets are handed out of a
 * private address space in page-sized slots, reused through a free list
 * that coalesces neighbouring ranges so a long-lived session does not
 * simply walk off the end of the space.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* --------------------------------------------------------- flink name table */

/* A flat table, not a hash: flink names are rare and there are few. */
#define GEM_MAX_NAMES 1024

struct gem_name_entry {
    uint32_t               name;
    struct drm_gem_object *obj;
};

static struct gem_name_entry gem_name_table[GEM_MAX_NAMES];
static uint32_t              gem_name_counter = 1;
static spinlock_t            gem_name_lock    = {0};

static struct drm_gem_object *gem_find_by_name(uint32_t name)
{
    int i;

    for (i = 0; i < GEM_MAX_NAMES; i++) {
        if (gem_name_table[i].obj != NULL && gem_name_table[i].name == name) { return gem_name_table[i].obj; }
    }
    return NULL;
}

static int gem_alloc_name(struct drm_gem_object *obj, uint32_t *name_out)
{
    int i;

    spin_lock(&gem_name_lock);

    for (i = 0; i < GEM_MAX_NAMES; i++) {
        if (gem_name_table[i].obj == NULL) { break; }
    }

    if (i >= GEM_MAX_NAMES) {
        spin_unlock(&gem_name_lock);
        return -ENOMEM;
    }

    gem_name_table[i].name = gem_name_counter;
    gem_name_table[i].obj  = obj;
    *name_out              = gem_name_counter;
    gem_name_counter++;

    spin_unlock(&gem_name_lock);
    return 0;
}

static void __attribute__((unused)) gem_free_name(uint32_t name)
{
    int i;

    spin_lock(&gem_name_lock);

    for (i = 0; i < GEM_MAX_NAMES; i++) {
        if (gem_name_table[i].name == name) {
            gem_name_table[i].name = 0;
            gem_name_table[i].obj  = NULL;
            break;
        }
    }

    spin_unlock(&gem_name_lock);
}

/* --------------------------------------------------- mmap offset allocator */

/*
 * Offsets live in a private 4 GiB window starting above 4 GiB, divided
 * into page-sized slots.  A bitmap says which slots are taken (so an
 * offset can be validated cheaply) and a sorted free list of ranges makes
 * reuse first-fit with coalescing on release.
 */
#define DUMB_OFFSET_SHIFT     12
#define DUMB_OFFSET_SIZE      (1ULL << DUMB_OFFSET_SHIFT)
#define DUMB_OFFSET_BASE      0x100000000ULL
#define DUMB_OFFSET_SPACE     0x100000000ULL
#define DUMB_OFFSET_MAX_SLOTS (DUMB_OFFSET_SPACE >> DUMB_OFFSET_SHIFT)
#define DUMB_BITMAP_SIZE      (DUMB_OFFSET_MAX_SLOTS / 8)

struct dumb_slot_range {
    uint32_t                start; /* first slot */
    uint32_t                count; /* contiguous slots */
    struct dumb_slot_range *next;
};

static uint8_t                 dumb_bitmap[DUMB_BITMAP_SIZE];
static struct dumb_slot_range *dumb_free_list;
static uint32_t                dumb_next_slot; /* never handed out so far */
static bool                    dumb_offset_inited = false;
static spinlock_t              dumb_alloc_lock    = {0};

/* Range nodes come from a fixed pool: they are allocated and freed in
 * the middle of mmap, where a failing malloc would be a shame. */
#define DUMB_RANGE_POOL_SIZE 256
static struct dumb_slot_range dumb_range_pool[DUMB_RANGE_POOL_SIZE];
static uint32_t               dumb_range_pool_used = 0;

static inline int dumb_bitmap_get(uint32_t slot)
{
    if (slot >= DUMB_OFFSET_MAX_SLOTS) { return -1; }
    return (dumb_bitmap[slot / 8] >> (slot % 8)) & 1;
}

static inline void dumb_bitmap_set(uint32_t slot)
{
    if (slot < DUMB_OFFSET_MAX_SLOTS) { dumb_bitmap[slot / 8] |= (uint8_t)(1U << (slot % 8)); }
}

static inline void dumb_bitmap_clear(uint32_t slot)
{
    if (slot < DUMB_OFFSET_MAX_SLOTS) { dumb_bitmap[slot / 8] &= (uint8_t) ~(1U << (slot % 8)); }
}

static struct dumb_slot_range *dumb_range_alloc_node(void)
{
    struct dumb_slot_range *r;

    if (dumb_range_pool_used < DUMB_RANGE_POOL_SIZE) {
        r = &dumb_range_pool[dumb_range_pool_used++];
    } else {
        r = malloc(sizeof(*r)); /* pool exhausted */
        if (r == NULL) { return NULL; }
    }

    memset(r, 0, sizeof(*r));
    return r;
}

static void dumb_range_free_node(struct dumb_slot_range *r)
{
    /* Pool nodes are never handed back; only heap ones can be freed. */
    if (r < dumb_range_pool || r >= dumb_range_pool + DUMB_RANGE_POOL_SIZE) { free(r); }
}

static void dumb_offset_init(void)
{
    memset(dumb_bitmap, 0, sizeof(dumb_bitmap));
    dumb_free_list       = NULL;
    dumb_next_slot       = 0;
    dumb_range_pool_used = 0;
    dumb_offset_inited   = true;
}

static inline uint32_t dumb_slots_needed(size_t size)
{
    return (uint32_t)((size + DUMB_OFFSET_SIZE - 1) >> DUMB_OFFSET_SHIFT);
}

static void dumb_mark_slots(uint32_t start, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) { dumb_bitmap_set(start + i); }
}

/* Hand out @size bytes' worth of offset space.  0 means "none left". */
static uint64_t dumb_offset_alloc(size_t size)
{
    uint32_t need = dumb_slots_needed(size);
    uint32_t start;

    if (need == 0) { need = 1; }

    if (!dumb_offset_inited) { dumb_offset_init(); }

    spin_lock(&dumb_alloc_lock);

    /* Reuse a released range if one is big enough. */
    {
        struct dumb_slot_range **prev = &dumb_free_list;
        struct dumb_slot_range  *cur  = dumb_free_list;

        while (cur != NULL) {
            if (cur->count >= need) {
                start = cur->start;
                if (cur->count == need) {
                    *prev = cur->next;
                    dumb_range_free_node(cur);
                } else {
                    cur->start += need;
                    cur->count -= need;
                }
                dumb_mark_slots(start, need);
                spin_unlock(&dumb_alloc_lock);
                return DUMB_OFFSET_BASE + ((uint64_t)start << DUMB_OFFSET_SHIFT);
            }
            prev = &cur->next;
            cur  = cur->next;
        }
    }

    /* Nothing to reuse: take it off the top. */
    if (dumb_next_slot + need > DUMB_OFFSET_MAX_SLOTS) {
        spin_unlock(&dumb_alloc_lock);
        return 0;
    }

    start = dumb_next_slot;
    dumb_next_slot += need;
    dumb_mark_slots(start, need);

    spin_unlock(&dumb_alloc_lock);
    return DUMB_OFFSET_BASE + ((uint64_t)start << DUMB_OFFSET_SHIFT);
}

/*
 * Give the slots back, keeping the free list sorted and coalescing with
 * whichever neighbours are already free so the space does not fragment
 * into unusable one-slot holes.
 */
static void dumb_offset_free(uint64_t offset, size_t size)
{
    uint32_t start, count, i;

    if (offset < DUMB_OFFSET_BASE) { return; }

    start = (uint32_t)((offset - DUMB_OFFSET_BASE) >> DUMB_OFFSET_SHIFT);
    count = dumb_slots_needed(size);
    if (count == 0) { count = 1; }

    spin_lock(&dumb_alloc_lock);

    for (i = 0; i < count && (start + i) < DUMB_OFFSET_MAX_SLOTS; i++) { dumb_bitmap_clear(start + i); }

    {
        struct dumb_slot_range **prev = &dumb_free_list;
        struct dumb_slot_range  *cur  = dumb_free_list;

        while (cur != NULL && cur->start < start) {
            prev = &cur->next;
            cur  = cur->next;
        }

        if (*prev != NULL && (*prev)->start + (*prev)->count == start) {
            (*prev)->count += count;
            if (cur != NULL && (*prev)->start + (*prev)->count == cur->start) {
                (*prev)->count += cur->count;
                (*prev)->next = cur->next;
                dumb_range_free_node(cur);
            }
        } else if (cur != NULL && start + count == cur->start) {
            cur->start = start;
            cur->count += count;
        } else {
            struct dumb_slot_range *fresh = dumb_range_alloc_node();

            if (fresh != NULL) {
                fresh->start = start;
                fresh->count = count;
                fresh->next  = cur;
                *prev        = fresh;
            }
        }
    }

    spin_unlock(&dumb_alloc_lock);
}

/* --------------------------------------------------------- object lifecycle */

int drm_gem_object_init(struct drm_device *dev, struct drm_gem_object *obj, size_t size)
{
    if (obj == NULL) { return -EINVAL; }

    obj->dev          = dev;
    obj->size         = (uint32_t)size;
    obj->refcount     = 1;
    obj->handle_count = 0;
    obj->ref_lock.v   = 0;

    return 0;
}

int drm_gem_private_object_init(struct drm_device *dev, struct drm_gem_object *obj, size_t size)
{
    return drm_gem_object_init(dev, obj, size);
}

void drm_gem_object_get(struct drm_gem_object *obj)
{
    if (obj == NULL) { return; }

    spin_lock(&obj->ref_lock);
    obj->refcount++;
    spin_unlock(&obj->ref_lock);
}

/*
 * Drop a reference, and on the last one let the memory go.  The decision
 * is made under the object's lock so that exactly one caller ever reaches
 * the teardown.
 */
void drm_gem_object_put(struct drm_gem_object *obj)
{
    int refcount;

    if (obj == NULL) { return; }

    spin_lock(&obj->ref_lock);
    refcount = --obj->refcount;
    spin_unlock(&obj->ref_lock);

    if (refcount != 0) { return; }

    if (obj->prime_fd > 0) {
        drm_gem_prime_fd_free(obj->prime_fd);
        obj->prime_fd = -1;
    }

    dumb_offset_free(obj->mmap_offset, obj->size);

    if (obj->dev != NULL && obj->dev->driver != NULL && obj->dev->driver->gem_free_object != NULL) {
        obj->dev->driver->gem_free_object(obj);
    } else {
        aligned_free(obj->backing);
        free(obj);
    }
}

/* ----------------------------------------------------------------- handles */

/*
 * Give @obj a handle in @file_priv.  The handle carries its own reference,
 * so the object outlives whoever created it for as long as the client
 * keeps the handle.
 */
int drm_gem_handle_create(struct drm_file *file_priv, struct drm_gem_object *obj, uint32_t *handle_out)
{
    struct drm_gem_handle_entry *entry;
    int                          ret;

    if (file_priv == NULL || obj == NULL || handle_out == NULL) { return -EINVAL; }

    entry = malloc(sizeof(*entry));
    if (entry == NULL) { return -ENOMEM; }
    memset(entry, 0, sizeof(*entry));

    spin_lock(&file_priv->table_lock);

    ret = drm_idr_alloc(&file_priv->object_idr, obj, 1, 0, handle_out);
    if (ret < 0) {
        spin_unlock(&file_priv->table_lock);
        free(entry);
        return ret;
    }

    entry->handle = *handle_out;
    entry->obj    = obj;

    drm_gem_object_get(obj);
    ilist_insert_after(&file_priv->object_list, &entry->head);
    obj->handle_count++;

    spin_unlock(&file_priv->table_lock);
    return 0;
}

int drm_gem_handle_delete(struct drm_file *file_priv, uint32_t handle)
{
    struct drm_gem_object *obj;

    if (file_priv == NULL) { return -EINVAL; }

    spin_lock(&file_priv->table_lock);

    obj = drm_idr_remove(&file_priv->object_idr, handle);
    if (obj != NULL) {
        ilist_node_t *node = file_priv->object_list.next;

        while (node != NULL && node != &file_priv->object_list) {
            struct drm_gem_handle_entry *entry = container_of(node, struct drm_gem_handle_entry, head);

            node = node->next;
            if (entry->handle == handle) {
                ilist_remove(&entry->head);
                free(entry);
                break;
            }
        }
        obj->handle_count--;
    }

    spin_unlock(&file_priv->table_lock);

    if (obj != NULL) { drm_gem_object_put(obj); }

    return (obj != NULL) ? 0 : -ENOENT;
}

struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file_priv, uint32_t handle)
{
    struct drm_gem_object *obj;

    if (file_priv == NULL) { return NULL; }

    spin_lock(&file_priv->table_lock);
    obj = drm_idr_find(&file_priv->object_idr, handle);
    if (obj != NULL) { drm_gem_object_get(obj); }
    spin_unlock(&file_priv->table_lock);

    return obj;
}

/* The offset form of the same lookup, used when a client mmaps. */
struct drm_gem_object *drm_gem_object_lookup_by_offset(struct drm_file *file_priv, uint64_t offset)
{
    struct drm_gem_object *obj = NULL;
    ilist_node_t          *node;

    if (file_priv == NULL) { return NULL; }

    spin_lock(&file_priv->table_lock);

    for (node = file_priv->object_list.next; node != NULL && node != &file_priv->object_list; node = node->next) {
        struct drm_gem_handle_entry *entry = container_of(node, struct drm_gem_handle_entry, head);

        if (entry->obj != NULL && entry->obj->mmap_offset == offset) {
            obj = entry->obj;
            drm_gem_object_get(obj);
            break;
        }
    }

    spin_unlock(&file_priv->table_lock);
    return obj;
}

/* ------------------------------------------------------------ dumb buffers */

/*
 * DRM_IOCTL_MODE_CREATE_DUMB: make a buffer of a given width, height and
 * depth.  Pitch is whatever the bytes-per-pixel arithmetic gives, rounded
 * up by the caller's own width, and the backing memory is page-aligned
 * because it gets mapped straight into user space.
 */
int drm_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev, struct drm_mode_create_dumb *args)
{
    struct drm_gem_object *obj;
    uint32_t               handle;
    size_t                 size;
    int                    ret;

    if (file_priv == NULL || dev == NULL || args == NULL) { return -EINVAL; }
    if (args->width == 0 || args->height == 0 || args->bpp == 0) { return -EINVAL; }
    if (args->bpp % 8 != 0) { return -EINVAL; }

    args->pitch = args->width * (args->bpp / 8);
    size        = (size_t)args->pitch * args->height;
    args->size  = (uint64_t)size;

    obj = malloc(sizeof(*obj));
    if (obj == NULL) { return -ENOMEM; }
    memset(obj, 0, sizeof(*obj));

    drm_gem_object_init(dev, obj, size);
    obj->size = (uint32_t)size;

    /* Assigned here rather than at mmap time so the offset is stable for
     * the life of the buffer: a client may ask for it more than once. */
    obj->mmap_offset = dumb_offset_alloc(size);

    if (size > 0) {
        obj->backing = aligned_alloc(4096, size);
        if (obj->backing == NULL) {
            free(obj);
            return -ENOMEM;
        }
        memset(obj->backing, 0, size);
    }

    obj->prime_fd = -1;

    ret = drm_gem_handle_create(file_priv, obj, &handle);
    if (ret < 0) {
        aligned_free(obj->backing);
        free(obj);
        return ret;
    }

    args->handle = handle;

    /* The handle owns the object from here on. */
    drm_gem_object_put(obj);

    return 0;
}

/* DRM_IOCTL_MODE_MAP_DUMB: hand back the offset the client should mmap. */
int drm_gem_dumb_map_offset(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle, uint64_t *offset)
{
    struct drm_gem_object *obj;

    (void)dev;

    obj = drm_gem_object_lookup(file_priv, handle);

    dbg_puts("DRMMAPDUMB: handle=");
    dbg_puts_dec(handle);
    dbg_puts(" obj=");
    dbg_puts_hex((uint64_t)(uintptr_t)obj);
    dbg_puts(" mmap_offset=");
    dbg_puts_hex((obj != NULL) ? obj->mmap_offset : 0);
    dbg_puts("\r\n");

    if (obj == NULL) { return -ENOENT; }

    *offset = obj->mmap_offset;

    drm_gem_object_put(obj);
    return 0;
}

/* DRM_IOCTL_MODE_DESTROY_DUMB: a dumb buffer is an ordinary GEM object,
 * so dropping its handle is the whole of it. */
int drm_gem_dumb_destroy(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle)
{
    (void)dev;

    return drm_gem_handle_delete(file_priv, handle);
}

/* ------------------------------------------------------------- GEM ioctls */

/* DRM_IOCTL_GEM_OPEN: import a buffer by its global name. */
int drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_gem_open   *args = (struct drm_gem_open *)data;
    struct drm_gem_object *obj;
    uint32_t               handle;
    int                    ret;

    (void)dev;

    spin_lock(&gem_name_lock);
    obj = gem_find_by_name(args->name);
    if (obj != NULL) { drm_gem_object_get(obj); }
    spin_unlock(&gem_name_lock);

    if (obj == NULL) { return -ENOENT; }

    ret = drm_gem_handle_create(file_priv, obj, &handle);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    args->handle = handle;
    args->size   = obj->size;

    /* The handle has its own reference; drop the one the name lookup took. */
    drm_gem_object_put(obj);
    return 0;
}

/* DRM_IOCTL_GEM_CLOSE. */
int drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_gem_close *args = (struct drm_gem_close *)data;

    (void)dev;

    return drm_gem_handle_delete(file_priv, args->handle);
}

/* DRM_IOCTL_GEM_FLINK: give a buffer a global name so another process can
 * open it. */
int drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_gem_flink  *args = (struct drm_gem_flink *)data;
    struct drm_gem_object *obj;
    uint32_t               name;
    int                    ret;

    (void)dev;

    obj = drm_gem_object_lookup(file_priv, args->handle);
    if (obj == NULL) { return -ENOENT; }

    ret = gem_alloc_name(obj, &name);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    args->name = name;

    drm_gem_object_put(obj);
    return 0;
}

/* ------------------------------------------- PRIME: sharing through an fd */

#define PRIME_FD_MAX 1024

static struct {
    struct drm_gem_object *obj;
    int                    in_use;
} prime_fd_table[PRIME_FD_MAX];

static spinlock_t prime_fd_lock = {0};

static int prime_fd_alloc(struct drm_gem_object *obj, int *fd_out)
{
    int i;

    spin_lock(&prime_fd_lock);

    for (i = 0; i < PRIME_FD_MAX; i++) {
        if (!prime_fd_table[i].in_use) {
            prime_fd_table[i].obj    = obj;
            prime_fd_table[i].in_use = 1;
            *fd_out                  = i + 1; /* fds count from 1 */
            obj->prime_fd            = *fd_out;
            drm_gem_object_get(obj);
            spin_unlock(&prime_fd_lock);
            return 0;
        }
    }

    spin_unlock(&prime_fd_lock);
    return -ENOMEM;
}

static struct drm_gem_object *prime_fd_lookup(int fd)
{
    struct drm_gem_object *obj = NULL;
    int                    idx = fd - 1;

    if (idx < 0 || idx >= PRIME_FD_MAX) { return NULL; }

    spin_lock(&prime_fd_lock);
    if (prime_fd_table[idx].in_use) {
        obj = prime_fd_table[idx].obj;
        if (obj != NULL) { drm_gem_object_get(obj); }
    }
    spin_unlock(&prime_fd_lock);

    return obj;
}

void drm_gem_prime_fd_free(int fd)
{
    int idx = fd - 1;

    if (idx < 0 || idx >= PRIME_FD_MAX) { return; }

    spin_lock(&prime_fd_lock);
    if (prime_fd_table[idx].in_use) {
        prime_fd_table[idx].obj    = NULL;
        prime_fd_table[idx].in_use = 0;
    }
    spin_unlock(&prime_fd_lock);
}

int drm_gem_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv, uint32_t handle, uint32_t flags,
                               int *prime_fd)
{
    struct drm_gem_object *obj;
    int                    fd;
    int                    ret;

    (void)dev;
    (void)flags;

    obj = drm_gem_object_lookup(file_priv, handle);
    if (obj == NULL) { return -ENOENT; }

    /* Exporting twice gives the same fd: two different numbers for one
     * buffer would only confuse the importer. */
    if (obj->prime_fd > 0) {
        *prime_fd = obj->prime_fd;
        drm_gem_object_put(obj);
        return 0;
    }

    ret = prime_fd_alloc(obj, &fd);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    *prime_fd = fd;
    drm_gem_object_put(obj);
    return 0;
}

int drm_gem_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv, int prime_fd, uint32_t *handle)
{
    struct drm_gem_object *obj;
    uint32_t               new_handle;
    int                    ret;

    (void)dev;

    obj = prime_fd_lookup(prime_fd);
    if (obj == NULL) { return -ENOENT; }

    ret = drm_gem_handle_create(file_priv, obj, &new_handle);
    if (ret < 0) {
        drm_gem_object_put(obj);
        return ret;
    }

    *handle = new_handle;
    drm_gem_object_put(obj);
    return 0;
}

/* Ensure @obj has an offset a client can mmap; idempotent. */
int drm_gem_create_mmap_offset(struct drm_gem_object *obj)
{
    if (obj == NULL || obj->size == 0) { return -EINVAL; }
    if (obj->mmap_offset != 0) { return 0; }

    obj->mmap_offset = dumb_offset_alloc(obj->size);
    return (obj->mmap_offset != 0) ? 0 : -ENOSPC;
}
