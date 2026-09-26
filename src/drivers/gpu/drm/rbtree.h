/*
 * rbtree.h — augmented red-black tree over intrusive nodes. (GPLv2)
 *
 * A self-balancing search tree for anything the DRM core needs to keep
 * sorted: interval nodes in the range allocator today, possibly more later.
 * Nodes carry no key of their own -- the caller compares two nodes with an
 * rb_less_fn, which keeps a single object on several trees or trees of
 * several kinds without any per-key storage.
 *
 * Two extras over a textbook tree:
 *
 *  - `leftmost` is cached in the root, so "smallest element" (the question
 *    asked at the start of every range search and every in-order walk) is
 *    answered without descending.
 *  - callers may pass an rb_augment_fn, called bottom-up along the path a
 *    change touched, letting them keep a per-subtree summary in
 *    `min_vruntime`.  The range allocator stashes its subtree's highest end
 *    address there.
 */

#ifndef INCLUDE_RBTREE_H_
#define INCLUDE_RBTREE_H_

#include <stddef.h>
#include <stdint.h>

typedef enum { RB_RED, RB_BLACK } rb_color_t;

typedef struct rb_node {
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
    uint64_t        min_vruntime; /* caller's per-subtree summary */
    rb_color_t      color;
} rb_node_t;

typedef struct {
    rb_node_t *root;
    rb_node_t *leftmost; /* smallest node, or NULL when empty */
} rb_root_t;

/* Strict weak ordering over two nodes; true when @a sorts before @b. */
typedef int (*rb_less_fn)(const rb_node_t *a, const rb_node_t *b);

/* Recompute one node's summary after the tree moved its children. */
typedef void (*rb_augment_fn)(rb_node_t *node, void *data);

#define RB_ROOT_INIT \
    {                \
        NULL, NULL   \
    }

/* Recover the object that owns a tree node: @ptr is an rb_node_t member
 * called @member inside @type. */
#define rb_entry(ptr, type, member) ((type *)((uint8_t *)(ptr) - offsetof(type, member)))

/* Make @root an empty tree. */
void rb_init_root(rb_root_t *root);

/*
 * Place @node in @root.  @less decides where it belongs and @augment (may
 * be NULL) refreshes summaries along the way.
 */
void rb_insert_augmented(rb_root_t *root, rb_node_t *node, rb_less_fn less, rb_augment_fn augment, void *data);

/*
 * Take @node out of @root.  @node must be in the tree; afterwards no tree
 * owns it and its links are meaningless to the caller.
 */
void rb_erase_augmented(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data);

/* Smallest node, or NULL. */
rb_node_t *rb_first(rb_root_t *root);

/* In-order successor of @node, or NULL when @node is the largest. */
rb_node_t *rb_next(rb_node_t *node);

/* Non-zero when the tree holds nothing. */
int rb_is_empty(rb_root_t *root);

#endif /* INCLUDE_RBTREE_H_ */
