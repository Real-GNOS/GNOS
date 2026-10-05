#ifndef IOQUEUE_H
#define IOQUEUE_H

#include "kernel/sched/sync.h"
#include "kernel/sched/thread.h"
#include <stdint.h>

#define BUFSIZE 1024

struct TTY_IOQUEUE {
    struct SCHED_LOCK lock;
    struct TASK *producer;
    struct TASK *consumer;
    uint32_t ends;
    char buf[BUFSIZE];
    int32_t head;
    int32_t tail;
};

void ioq_init(struct TTY_IOQUEUE *ioq);
int ioq_full(struct TTY_IOQUEUE *ioq);
int ioq_empty(struct TTY_IOQUEUE *ioq);
char ioq_getchar(struct TTY_IOQUEUE *ioq);
void ioq_putchar(struct TTY_IOQUEUE *ioq, char byte);
uint32_t ioq_length(struct TTY_IOQUEUE *ioq);

#endif
