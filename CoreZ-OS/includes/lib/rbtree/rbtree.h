#ifndef RBTREE_H
#define RBTREE_H

#include <stdint.h>
#include <stddef.h>

enum RB_COLOR {
    RB_RED,
    RB_BLACK
};

struct RB_NODE {
    struct RB_NODE *parent;
    struct RB_NODE *left;
    struct RB_NODE *right;
    enum RB_COLOR color;
    uint64_t key;
};

struct RB_ROOT {
    struct RB_NODE *root;
};

static inline void rb_root_init(struct RB_ROOT *root) {
    root->root = NULL;
}

static inline void rb_node_init(struct RB_NODE *node) {
    node->parent = node->left = node->right = NULL;
    node->color = RB_RED;
}

#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

struct RB_NODE *rb_insert(struct RB_ROOT *root, struct RB_NODE *node);
void rb_erase(struct RB_ROOT *root, struct RB_NODE *node);
struct RB_NODE *rb_first(struct RB_ROOT *root);
struct RB_NODE *rb_find(struct RB_ROOT *root, uint64_t key);

static inline int rb_empty(struct RB_ROOT *root) {
    return root->root == NULL;
}

#endif
