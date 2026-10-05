#include "lib/list/list.h"

void list_init(struct LIST *list) {
    list->head.prev = 0;
    list->head.next = &list->tail;
    list->tail.prev = &list->head;
    list->tail.next = 0;
}

void list_append(struct LIST *list, struct LIST_ELEM *elem) {
    elem->next = &list->tail;
    elem->prev = list->tail.prev;
    list->tail.prev->next = elem;
    list->tail.prev = elem;
}

void list_remove(struct LIST_ELEM *elem) {
    elem->prev->next = elem->next;
    elem->next->prev = elem->prev;
}

void list_unlink(struct LIST_ELEM *elem) {
    if (elem->next != 0) {
        list_remove(elem);
        elem->next = 0;
        elem->prev = 0;
    }
}

int list_empty(struct LIST *list) {
    return list->head.next == &list->tail;
}

struct LIST_ELEM *list_pop_front(struct LIST *list) {
    struct LIST_ELEM *first = list->head.next;
    if (first == &list->tail) {
        return 0;
    }
    list_remove(first);
    return first;
}

struct LIST_ELEM *elem_find(struct LIST *list, struct LIST_ELEM *elem) {
    struct LIST_ELEM *e = list->head.next;
    while (e != &list->tail) {
        if (e == elem) {
            return e;
        }
        e = e->next;
    }
    return 0;
}
