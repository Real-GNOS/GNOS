/*
 * drm_hashtab.h — keyed lookups over a fixed bucket array. (GPLv2)
 *
 * This is the smallest possible thing that answers "is this magic token
 * known to this file?" — which is exactly what DRM authentication asks.  A
 * caller-supplied order fixes the bucket count at creation; keys collide
 * into a per-bucket intrusive list.  Callers own the items, so inserting
 * and removing never allocate and never free.
 */

#ifndef INCLUDE_DRM_DRM_HASHTAB_H_
#define INCLUDE_DRM_DRM_HASHTAB_H_

#include <stddef.h>
#include <stdint.h>

#include "intrusive_list.h"

/* What a caller embeds to make its own objects hashable. */
struct drm_hash_item {
    ilist_node_t  link;
    unsigned long key;
};

struct drm_open_hash {
    unsigned int  size;  /* bucket count, always 2^order */
    unsigned int  order; /* log2(size) */
    ilist_node_t *table; /* bucket array, each one a list sentinel */
};

/* Build a table with 2^@order buckets.  Returns 0, or -ENOMEM. */
int drm_ht_create(struct drm_open_hash *ht, unsigned int order);

/* Drop the table's bucket array.  Items are the caller's and are untouched. */
void drm_ht_destroy(struct drm_open_hash *ht);

/* Chain @item under item->key.  Returns 0, or -EINVAL if the key is taken. */
int drm_ht_insert_item(struct drm_open_hash *ht, struct drm_hash_item *item);

/*
 * Look up (*item)->key and, if a matching item exists, repoint *item at it.
 * Returns 0, or -EINVAL when nothing carries that key.
 */
int drm_ht_peek(struct drm_open_hash *ht, struct drm_hash_item **item);

/* Look @key up and report the item in *@item.  Returns 0, or -EINVAL. */
int drm_ht_find_item(struct drm_open_hash *ht, unsigned long key, struct drm_hash_item **item);

/*
 * Unchain @item.  Returns 0, or -EINVAL when @item was already detached
 * (its links are blanked on removal precisely so this can be told apart).
 */
int drm_ht_remove_item(struct drm_open_hash *ht, struct drm_hash_item *item);

#endif /* INCLUDE_DRM_DRM_HASHTAB_H_ */
