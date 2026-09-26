/* SPDX-License-Identifier: GPL-2.0 */
/*
 * intrusive_list.c — circular doubly-linked lists of embedded nodes. (GPLv2)
 *
 * Every operation is a handful of pointer writes; the whole file exists so
 * the rest of the DRM core never has to spell out link surgery twice.  See
 * intrusive_list.h for what the return values mean.
 */

#include <stddef.h>

#include "intrusive_list.h"

int ilist_init(struct ilist_node *list)
{
    if (list == NULL) { return 1; }

    list->prev = list;
    list->next = list;
    return 0;
}

int ilist_insert_after(struct ilist_node *node, struct ilist_node *new_node)
{
    if (node == NULL || new_node == NULL) { return 1; }

    new_node->prev   = node;
    new_node->next   = node->next;
    node->next->prev = new_node;
    node->next       = new_node;
    return 0;
}

int ilist_insert_before(struct ilist_node *node, struct ilist_node *new_node)
{
    if (node == NULL || new_node == NULL) { return 1; }

    /* Behind @node is the same thing as in front of the node before it. */
    return ilist_insert_after(node->prev, new_node);
}

int ilist_remove(struct ilist_node *node)
{
    if (node == NULL) { return 1; }

    /* Three ways for a node to have nothing to unlink from: it was never
     * chained up (NULL), it was already taken off (NULL again, because the
     * removal below blanks both links), or it is a lone sentinel pointing
     * at itself. Refusing all three is what makes "remove it twice" a
     * harmless no-op for callers instead of a wild pointer write. */
    if (node->prev == NULL || node->next == NULL) { return 1; }
    if (node->next == node || node->prev == node) { return 1; }

    node->prev->next = node->next;
    node->next->prev = node->prev;

    /* Leave an obvious "detached" signature behind. */
    node->prev = 0;
    node->next = 0;
    return 0;
}

int ilist_is_empty(const struct ilist_node *list)
{
    if (list == NULL) { return 1; }

    return list->next == list;
}
