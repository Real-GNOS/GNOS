#include "lib/rbtree/rbtree.h"

static void rb_rotate_left(struct RB_ROOT *root, struct RB_NODE *node) {
    struct RB_NODE *right = node->right;
    node->right = right->left;
    if (right->left)
        right->left->parent = node;
    right->parent = node->parent;
    if (node->parent == NULL)
        root->root = right;
    else if (node == node->parent->left)
        node->parent->left = right;
    else
        node->parent->right = right;
    right->left = node;
    node->parent = right;
}

static void rb_rotate_right(struct RB_ROOT *root, struct RB_NODE *node) {
    struct RB_NODE *left = node->left;
    node->left = left->right;
    if (left->right)
        left->right->parent = node;
    left->parent = node->parent;
    if (node->parent == NULL)
        root->root = left;
    else if (node == node->parent->left)
        node->parent->left = left;
    else
        node->parent->right = left;
    left->right = node;
    node->parent = left;
}

static void rb_insert_fixup(struct RB_ROOT *root, struct RB_NODE *node) {
    while (node->parent && node->parent->color == RB_RED) {
        struct RB_NODE *grandparent = node->parent->parent;
        if (node->parent == grandparent->left) {
            struct RB_NODE *uncle = grandparent->right;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                grandparent->color = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    rb_rotate_left(root, node);
                }

                node->parent->color = RB_BLACK;
                grandparent->color = RB_RED;
                rb_rotate_right(root, grandparent);
            }
        } else {
            struct RB_NODE *uncle = grandparent->left;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                grandparent->color = RB_RED;
                node = grandparent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    rb_rotate_right(root, node);
                }
                node->parent->color = RB_BLACK;
                grandparent->color = RB_RED;
                rb_rotate_left(root, grandparent);
            }
        }
    }
    root->root->color = RB_BLACK;
}

struct RB_NODE *rb_insert(struct RB_ROOT *root, struct RB_NODE *node) {
    if (root->root == NULL) {
        root->root = node;
        node->parent = node->left = node->right = NULL;
        node->color = RB_BLACK;
        return NULL;
    }

    struct RB_NODE *cur = root->root;
    struct RB_NODE *parent = NULL;
    while (cur) {
        parent = cur;
        if (node->key < cur->key)
            cur = cur->left;
        else if (node->key > cur->key)
            cur = cur->right;
        else
            return cur;
    }

    node->parent = parent;
    node->left = node->right = NULL;
    node->color = RB_RED;

    if (node->key < parent->key)
        parent->left = node;
    else
        parent->right = node;

    rb_insert_fixup(root, node);
    return NULL;
}

static void rb_erase_fixup(struct RB_ROOT *root, struct RB_NODE *node,
                           struct RB_NODE *parent) {
    (void)root;
    while ((node == NULL || node->color == RB_BLACK) && parent != NULL) {
        if (parent->left == node) {
            struct RB_NODE *sibling = parent->right;
            if (sibling != NULL && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_left(root, parent);
                sibling = parent->right;
            }
            if ((sibling == NULL || sibling->left == NULL ||
                 sibling->left->color == RB_BLACK) &&
                (sibling == NULL || sibling->right == NULL ||
                 sibling->right->color == RB_BLACK)) {
                if (sibling != NULL)
                    sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (sibling == NULL || sibling->right == NULL ||
                    sibling->right->color == RB_BLACK) {
                    if (sibling != NULL && sibling->left != NULL)
                        sibling->left->color = RB_BLACK;
                    if (sibling != NULL)
                        sibling->color = RB_RED;
                    rb_rotate_right(root, sibling);
                    sibling = parent->right;
                }
                if (sibling != NULL)
                    sibling->color = parent->color;
                parent->color = RB_BLACK;
                if (sibling != NULL && sibling->right != NULL)
                    sibling->right->color = RB_BLACK;
                rb_rotate_left(root, parent);
                node = root->root;
                break;
            }
        } else {
            struct RB_NODE *sibling = parent->left;
            if (sibling != NULL && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_right(root, parent);
                sibling = parent->left;
            }
            if ((sibling == NULL || sibling->left == NULL ||
                 sibling->left->color == RB_BLACK) &&
                (sibling == NULL || sibling->right == NULL ||
                 sibling->right->color == RB_BLACK)) {
                if (sibling != NULL)
                    sibling->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (sibling == NULL || sibling->left == NULL ||
                    sibling->left->color == RB_BLACK) {
                    if (sibling != NULL && sibling->right != NULL)
                        sibling->right->color = RB_BLACK;
                    if (sibling != NULL)
                        sibling->color = RB_RED;
                    rb_rotate_left(root, sibling);
                    sibling = parent->left;
                }
                if (sibling != NULL)
                    sibling->color = parent->color;
                parent->color = RB_BLACK;
                if (sibling != NULL && sibling->left != NULL)
                    sibling->left->color = RB_BLACK;
                rb_rotate_right(root, parent);
                node = root->root;
                break;
            }
        }
    }
    if (node != NULL)
        node->color = RB_BLACK;
}

void rb_erase(struct RB_ROOT *root, struct RB_NODE *node) {
    struct RB_NODE *child, *parent;
    struct RB_NODE *fix_parent;
    enum RB_COLOR color;

    if (node->left == NULL)
        child = node->right;
    else if (node->right == NULL)
        child = node->left;
    else {
        struct RB_NODE *old = node;
        struct RB_NODE *successor = node->right;
        while (successor->left != NULL)
            successor = successor->left;

        color = successor->color;
        child = successor->right;
        parent = successor->parent;

        if (child != NULL)
            child->parent = parent;

        if (parent == old)
            parent->right = child;
        else
            parent->left = child;

        successor->parent = old->parent;
        successor->color = old->color;
        successor->left = old->left;
        successor->right = old->right;

        if (old->parent != NULL) {
            if (old == old->parent->left)
                old->parent->left = successor;
            else
                old->parent->right = successor;
        } else {
            root->root = successor;
        }

        if (successor->left != NULL)
            successor->left->parent = successor;
        if (successor->right != NULL)
            successor->right->parent = successor;

        old->parent = old->left = old->right = NULL;
        fix_parent = (parent == old) ? successor : parent;
        goto fixup;
    }

    color = node->color;
    parent = node->parent;
    if (child != NULL)
        child->parent = parent;
    if (parent != NULL) {
        if (parent->left == node)
            parent->left = child;
        else
            parent->right = child;
    } else {
        root->root = child;
    }
    fix_parent = parent;

fixup:
    if (color == RB_BLACK)
        rb_erase_fixup(root, child, fix_parent);

    node->parent = node->left = node->right = NULL;
}

struct RB_NODE *rb_first(struct RB_ROOT *root) {
    struct RB_NODE *cur = root->root;
    while (cur && cur->left)
        cur = cur->left;
    return cur;
}

struct RB_NODE *rb_find(struct RB_ROOT *root, uint64_t key) {
    struct RB_NODE *cur = root->root;
    while (cur) {
        if (key < cur->key)
            cur = cur->left;
        else if (key > cur->key)
            cur = cur->right;
        else
            return cur;
    }
    return NULL;
}
