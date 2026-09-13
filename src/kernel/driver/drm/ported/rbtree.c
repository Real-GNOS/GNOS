/*
 * rbtree.c - augmented red-black tree over intrusive nodes. (GPLv2)
 *
 * A red-black tree keeps itself roughly balanced by colouring every node
 * red or black and insisting that (a) the root and every leaf are black,
 * (b) no red node has a red child, and (c) every path down from a node to
 * the leaves crosses the same number of black nodes.  Insertions and
 * removals break those rules locally, and the two fixup routines below put
 * them back with recolourings and rotations: never more than O(log n) of
 * them, and amortised almost none at all.
 *
 * Nodes are intrusive: nothing is copied or allocated here, the tree only
 * rearranges pointers, so anything a caller embedded an rb_node_t in can
 * sit in a tree and keep a stable address.
 *
 * Leaves are represented by one shared sentinel node rather than by NULL.
 * That is not decoration: during a deletion the *absence* of a node still
 * has a position (left or right child of something) that the rebalancing
 * has to know, and a sentinel can remember which side it stands on where a
 * NULL cannot.
 */

#include <stddef.h>
#include <stdint.h>

#include "rbtree.h"

/* The one and only leaf.  Its own pointers fold back on itself so that a
 * careless descent cannot run off; nothing writes to it except its parent
 * link, transiently, while a deletion is being repaired. */
static rb_node_t rb_sentinel = {
    .parent = &rb_sentinel, .left = &rb_sentinel, .right = &rb_sentinel, .min_vruntime = 0, .color = RB_BLACK
};

#define RB_NIL (&rb_sentinel)

/* ------------------------------------------------------------- small tools */

/* Refresh @augment from @node up to the root. */
static void rb_refresh_path(rb_node_t *node, rb_augment_fn augment, void *data)
{
    while (node != RB_NIL) {
        augment(node, data);
        node = node->parent;
    }
}

/* Leftmost node of a (possibly empty) subtree. */
static rb_node_t *rb_leftmost_of(rb_node_t *node)
{
    while (node->left != RB_NIL) { node = node->left; }
    return node;
}

/*
 * Replace subtree @old with subtree @new, which may be the sentinel.
 *
 *     pivot                  pivot
 *       |                      |
 *      old       becomes      new
 *     /   \                  /   \
 *    A     B                A     B
 */
static void rb_graft(rb_root_t *root, rb_node_t *old, rb_node_t *new_node)
{
    if (old->parent == RB_NIL) {
        root->root = new_node;
    } else if (old == old->parent->left) {
        old->parent->left = new_node;
    } else {
        old->parent->right = new_node;
    }

    new_node->parent = old->parent;
}

/*
 *     x                y
 *    / \              / \
 *   A   y     ->     x   C
 *      / \          / \
 *     B   C        A   B
 */
static void rb_pivot_left(rb_root_t *root, rb_node_t *x, rb_augment_fn augment, void *data)
{
    rb_node_t *y = x->right;

    x->right = y->left;
    y->left->parent = x;

    rb_graft(root, x, y);

    y->left   = x;
    x->parent = y;

    if (augment != NULL) {
        augment(x, data); /* x dropped below y: redo its summary first */
        augment(y, data);
    }
}

/* Mirror image of rb_pivot_left. */
static void rb_pivot_right(rb_root_t *root, rb_node_t *x, rb_augment_fn augment, void *data)
{
    rb_node_t *y = x->left;

    x->left = y->right;
    y->right->parent = x;

    rb_graft(root, x, y);

    y->right  = x;
    x->parent = y;

    if (augment != NULL) {
        augment(x, data);
        augment(y, data);
    }
}

/* ------------------------------------------------------------ insert fixup */

/* Fresh nodes arrive red, which can put two reds in a row. */
static void rb_insert_repair(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data)
{
    while (node != root->root && node->parent->color == RB_RED) {
        rb_node_t *parent = node->parent;
        rb_node_t *uncle;
        int        parent_on_left = (parent == parent->parent->left);

        uncle = parent_on_left ? parent->parent->right : parent->parent->left;

        if (uncle->color == RB_RED) {
            /* A red uncle can be fixed by recolouring alone: blackness
             * moves down one level and the violation moves up to the
             * grandparent, which the loop takes over. */
            parent->color         = RB_BLACK;
            uncle->color          = RB_BLACK;
            parent->parent->color = RB_RED;
            node                  = parent->parent;
            continue;
        }

        if (parent_on_left) {
            if (node == parent->right) {
                /* The node hangs the wrong way for a single rotation, so
                 * turn it into the mirror case first. */
                rb_pivot_left(root, parent, augment, data);
                node = parent;
            }
            node->parent->color         = RB_BLACK;
            node->parent->parent->color = RB_RED;
            rb_pivot_right(root, node->parent->parent, augment, data);
        } else {
            if (node == parent->left) {
                rb_pivot_right(root, parent, augment, data);
                node = parent;
            }
            node->parent->color         = RB_BLACK;
            node->parent->parent->color = RB_RED;
            rb_pivot_left(root, node->parent->parent, augment, data);
        }
        break;
    }

    root->root->color = RB_BLACK;
}

/* ------------------------------------------------------------- erase fixup */

/*
 * @x stands where a black node was taken away, so every path through it is
 * one black short.  Recolouring the sibling (and rotating when its children
 * allow it) pushes that shortage upward until it either cancels against a
 * red node or reaches the root, where one black fewer on every path is
 * simply legal.
 */
static void rb_erase_repair(rb_root_t *root, rb_node_t *x, rb_augment_fn augment, void *data)
{
    while (x != root->root && x->color == RB_BLACK) {
        rb_node_t *parent    = x->parent;
        rb_node_t *sibling;
        int        x_on_left = (x == parent->left);

        sibling = x_on_left ? parent->right : parent->left;

        if (sibling->color == RB_RED) {
            /* A red sibling cannot give up a black node directly; one
             * rotation makes it black with a red child to borrow from. */
            sibling->color = RB_BLACK;
            parent->color  = RB_RED;
            if (x_on_left) {
                rb_pivot_left(root, parent, augment, data);
            } else {
                rb_pivot_right(root, parent, augment, data);
            }
            sibling = x_on_left ? parent->right : parent->left;
        }

        if (sibling->left->color == RB_BLACK && sibling->right->color == RB_BLACK) {
            /* Nothing to borrow: redden the sibling so both of @parent's
             * sides are equally short and carry the shortage upward. */
            sibling->color = RB_RED;
            x              = parent;
            continue;
        }

        if (x_on_left) {
            if (sibling->right->color == RB_BLACK) {
                sibling->left->color = RB_BLACK;
                sibling->color       = RB_RED;
                rb_pivot_right(root, sibling, augment, data);
                sibling = parent->right;
            }
            sibling->color        = parent->color;
            parent->color         = RB_BLACK;
            sibling->right->color = RB_BLACK;
            rb_pivot_left(root, parent, augment, data);
        } else {
            if (sibling->left->color == RB_BLACK) {
                sibling->right->color = RB_BLACK;
                sibling->color        = RB_RED;
                rb_pivot_left(root, sibling, augment, data);
                sibling = parent->left;
            }
            sibling->color       = parent->color;
            parent->color        = RB_BLACK;
            sibling->left->color = RB_BLACK;
            rb_pivot_right(root, parent, augment, data);
        }
        break;
    }

    x->color = RB_BLACK;
}

/* ------------------------------------------------------------------ public */

void rb_init_root(rb_root_t *root)
{
    root->root     = RB_NIL;
    root->leftmost = RB_NIL;
}

rb_node_t *rb_first(rb_root_t *root)
{
    return (root->leftmost == RB_NIL) ? NULL : root->leftmost;
}

rb_node_t *rb_next(rb_node_t *node)
{
    rb_node_t *parent;

    if (node == NULL) { return NULL; }
    if (node->right != RB_NIL) { return rb_leftmost_of(node->right); }

    /* Climb until coming up out of a left subtree: that ancestor is the
     * next node in order. */
    parent = node->parent;
    while (parent != RB_NIL && node == parent->right) {
        node   = parent;
        parent = parent->parent;
    }
    return (parent == RB_NIL) ? NULL : parent;
}

int rb_is_empty(rb_root_t *root)
{
    return root->root == RB_NIL;
}

void rb_insert_augmented(rb_root_t *root, rb_node_t *node, rb_less_fn less, rb_augment_fn augment, void *data)
{
    rb_node_t *parent = RB_NIL;
    rb_node_t *walk   = root->root;

    while (walk != RB_NIL) {
        parent = walk;
        walk   = less(node, walk) ? walk->left : walk->right;
    }

    node->parent       = parent;
    node->left         = RB_NIL;
    node->right        = RB_NIL;
    node->color        = RB_RED;
    node->min_vruntime = 0;

    if (parent == RB_NIL) {
        root->root = node;
    } else if (less(node, parent)) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    if (root->leftmost == RB_NIL || less(node, root->leftmost)) { root->leftmost = node; }

    rb_insert_repair(root, node, augment, data);

    if (augment != NULL) { rb_refresh_path(node, augment, data); }
}

void rb_erase_augmented(rb_root_t *root, rb_node_t *node, rb_augment_fn augment, void *data)
{
    rb_node_t *heir;           /* what took its place, maybe the sentinel */
    rb_color_t detached_color;
    rb_node_t *refresh_from;

    if (root->leftmost == node) { root->leftmost = rb_next(node); }

    detached_color = node->color;

    if (node->left == RB_NIL) {
        heir = node->right;
        rb_graft(root, node, heir);
    } else if (node->right == RB_NIL) {
        heir = node->left;
        rb_graft(root, node, heir);
    } else {
        /* Two children: the in-order successor has room to spare (it has no
         * left child of its own), so it inherits this node's position,
         * children and colour. */
        rb_node_t *successor = rb_leftmost_of(node->right);

        detached_color = successor->color;
        heir           = successor->right;

        if (successor->parent == node) {
            /* Sentinel included: it must remember it now hangs off the
             * successor, which is about to take @node's place. */
            heir->parent = successor;
        } else {
            rb_graft(root, successor, successor->right);
            successor->right         = node->right;
            successor->right->parent = successor;
        }

        rb_graft(root, node, successor);

        successor->left         = node->left;
        successor->left->parent = successor;
        successor->color        = node->color;
    }

    /* Summaries along the changed path need redoing: once for what moved,
     * and again below for whatever the repair rotates. */
    refresh_from = (heir != RB_NIL) ? heir : heir->parent;
    if (augment != NULL && refresh_from != RB_NIL) { rb_refresh_path(refresh_from, augment, data); }

    if (detached_color == RB_BLACK) { rb_erase_repair(root, heir, augment, data); }

    if (augment != NULL && root->root != RB_NIL) { rb_refresh_path(root->root, augment, data); }

    /* Leave the sentinel tidy: a stale parent here is the sort of thing
     * that silently misdirects the next deletion. */
    rb_sentinel.parent = RB_NIL;
    node->left         = RB_NIL;
    node->right        = RB_NIL;
}
