#ifndef LIST_H
#define LIST_H

#include <stddef.h>
#include <stdint.h>

struct LIST_ELEM {
    struct LIST_ELEM *prev;
    struct LIST_ELEM *next;
};

struct LIST {
    struct LIST_ELEM head;
    struct LIST_ELEM tail;
};

void list_init(struct LIST *list);
void list_append(struct LIST *list, struct LIST_ELEM *elem);
void list_remove(struct LIST_ELEM *elem);
int list_empty(struct LIST *list);
void list_unlink(struct LIST_ELEM *elem);
struct LIST_ELEM *list_pop_front(struct LIST *list);
struct LIST_ELEM *elem_find(struct LIST *list, struct LIST_ELEM *elem);

#define list_entry(ptr, type, member)                                          \
    ((type *)((uintptr_t)(ptr) - offsetof(type, member)))

#endif
