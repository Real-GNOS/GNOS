/*
 * intrusive_list.h — circular doubly-linked lists of embedded nodes. (GPLv2)
 *
 * The list owns no nodes: each list is a sentinel that points at its first
 * and last entry, and every participant keeps an ilist_node_t inside itself.
 * That is what lets one object sit on several lists at once (a DRM mode
 * object is on the device's CRTC list and in the id tables at the same time)
 * and it means walking a list involves no allocation at all.
 *
 * A node that is not on any list is recognisable: removing a node zeroes its
 * two pointers, so "prev == NULL" answers "is this node free?".
 */

#ifndef INCLUDE_INTRUSIVE_LIST_H_
#define INCLUDE_INTRUSIVE_LIST_H_

typedef struct ilist_node {
    struct ilist_node *prev;
    struct ilist_node *next;
} ilist_node_t;

/* Make @list an empty ring: it points at itself in both directions.
 * Returns 0, or 1 when @list is NULL. */
int ilist_init(struct ilist_node *list);

/* Splice @new_node in just after @node.  Returns 0, or 1 on NULL input. */
int ilist_insert_after(struct ilist_node *node, struct ilist_node *new_node);

/* Splice @new_node in just before @node.  Returns 0, or 1 on NULL input. */
int ilist_insert_before(struct ilist_node *node, struct ilist_node *new_node);

/*
 * Unlink @node and blank its pointers.
 * Fails (returns 1) when @node is NULL or is not chained to anything else:
 * both a lone sentinel and an already-removed node look like that, which is
 * how callers turn "remove the object I already removed" into a no-op.
 */
int ilist_remove(struct ilist_node *node);

/* Non-zero when @list holds no entries (or @list is NULL). */
int ilist_is_empty(const struct ilist_node *list);

#endif /* INCLUDE_INTRUSIVE_LIST_H_ */
