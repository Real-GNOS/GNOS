/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_hashtab.c ¡ª keyed lookups over a fixed bucket array. (GPLv2)
 *
 * Sixteen buckets and usually a couple of items: this table exists only for
 * the auth magic-to-file linkage, so the interesting properties are that it
 * cannot fail after creation, that it allocates nothing per item, and that
 * removing something twice is detected rather than corrupting a list.
 */

#include "drm_port.h"
#include "drm_hashtab.h"

/* Recover the surrounding item from one of its list links. */
#define ht_item(node) ((struct drm_hash_item *)((uint8_t *)(node) - offsetof(struct drm_hash_item, link)))

/* Spread a key over the buckets: multiply by the 64-bit golden-ratio
 * constant and keep the top 'order' bits, which is the whole point of the
 * multiplication -- the low bits of adjacent keys are the bits that are
 * least alike. */
#define HT_GOLDEN 0x9e370001UL

static unsigned int ht_bucket(const struct drm_open_hash *ht, unsigned long key)
{
    if (ht->order == 0U) { return 0U; }
    return (unsigned int)((key * HT_GOLDEN) >> (64U - ht->order));
}

/* Walk @head's chain for @key.  Returns NULL when it is not there. */
static struct drm_hash_item *ht_chain_find(const ilist_node_t *head, unsigned long key)
{
    const ilist_node_t *cur;

    for (cur = head->next; cur != head; cur = cur->next) {
        struct drm_hash_item *item = ht_item(cur);

        if (item->key == key) { return item; }
    }
    return NULL;
}

int drm_ht_create(struct drm_open_hash *ht, unsigned int order)
{
    unsigned int i;

    /* Beyond this the bucket count stops fitting in an unsigned int, which
     * would make every later index arithmetic meaningless. */
    if (order >= (sizeof(unsigned int) * 8U)) { return -EINVAL; }

    ht->size  = 1U << order;
    ht->order = order;
    ht->table = malloc(ht->size * sizeof(ilist_node_t));
    if (ht->table == NULL) { return -ENOMEM; }

    for (i = 0; i < ht->size; i++) { ilist_init(&ht->table[i]); }

    return 0;
}

void drm_ht_destroy(struct drm_open_hash *ht)
{
    free(ht->table);
    memset(ht, 0, sizeof(*ht));
}

int drm_ht_insert_item(struct drm_open_hash *ht, struct drm_hash_item *item)
{
    ilist_node_t *head;

    if (item == NULL) { return -EINVAL; }

    head = &ht->table[ht_bucket(ht, item->key)];

    /* One key stands for exactly one item, so a second insert of the same
     * key has to be refused rather than silently shadowed. */
    if (ht_chain_find(head, item->key) != NULL) { return -EINVAL; }

    ilist_insert_after(head, &item->link);
    return 0;
}

int drm_ht_peek(struct drm_open_hash *ht, struct drm_hash_item **item)
{
    struct drm_hash_item *found;

    if (item == NULL || *item == NULL) { return -EINVAL; }

    found = ht_chain_find(&ht->table[ht_bucket(ht, (*item)->key)], (*item)->key);
    if (found == NULL) { return -EINVAL; }

    *item = found;
    return 0;
}

int drm_ht_find_item(struct drm_open_hash *ht, unsigned long key, struct drm_hash_item **item)
{
    struct drm_hash_item *found;

    found = ht_chain_find(&ht->table[ht_bucket(ht, key)], key);
    if (found == NULL) { return -EINVAL; }

    if (item != NULL) { *item = found; }
    return 0;
}

int drm_ht_remove_item(struct drm_open_hash *ht, struct drm_hash_item *item)
{
    (void)ht;

    if (item == NULL) { return -EINVAL; }
    if (item->link.prev == NULL) { return -EINVAL; }

    ilist_remove(&item->link);
    return 0;
}
