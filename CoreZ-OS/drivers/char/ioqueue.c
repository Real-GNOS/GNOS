#include "drivers/char/ioqueue.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "kernel/sched/sync.h"
void ioq_init(struct TTY_IOQUEUE *ioq) {
    lock_init(&ioq->lock);
    ioq->producer = 0;
    ioq->consumer = 0;
    ioq->head = 0;
    ioq->tail = 0;
}

static int32_t next_pos(int32_t pos) {
    return (pos + 1) % BUFSIZE;
}

int ioq_full(struct TTY_IOQUEUE *ioq) {
    return next_pos(ioq->head) == ioq->tail;
}

int ioq_empty(struct TTY_IOQUEUE *ioq) {
    return ioq->head == ioq->tail;
}

uint32_t ioq_length(struct TTY_IOQUEUE *ioq) {
    uint32_t len = 0;
    if (ioq->head >= ioq->tail) {
        len = (uint32_t)(ioq->head - ioq->tail);
    } else {
        len = (uint32_t)(BUFSIZE - (ioq->tail - ioq->head));
    }
    return len;
}

static uint32_t ioq_wait(struct TASK **waiter) {
    *waiter = current;
    return thread_block_prepare(TASK_BLOCKED);
}

static void wakeup(struct TASK **waiter) {
    struct TASK *w = *waiter;
    *waiter = 0;
    if (w) {
        thread_unblock(w);
    }
}

void ioq_putchar(struct TTY_IOQUEUE *ioq, char byte) {
    ASSERT((asm_save_eflags() & 0x200) == 0);
    lock_acquire(&ioq->lock);
    while (ioq_full(ioq)) {
        uint32_t bf = ioq_wait(&ioq->producer);
        lock_release(&ioq->lock);
        thread_block_commit(bf);
        lock_acquire(&ioq->lock);
    }
    ioq->buf[ioq->head] = byte;
    ioq->head = next_pos(ioq->head);
    if (ioq->consumer != 0) {
        wakeup(&ioq->consumer);
    }
    lock_release(&ioq->lock);
}

char ioq_getchar(struct TTY_IOQUEUE *ioq) {
    ASSERT((asm_save_eflags() & 0x200) == 0);
    lock_acquire(&ioq->lock);
    while (ioq_empty(ioq)) {
        uint32_t bf = ioq_wait(&ioq->consumer);
        lock_release(&ioq->lock);
        thread_block_commit(bf);
        lock_acquire(&ioq->lock);
    }
    char byte = ioq->buf[ioq->tail];
    ioq->tail = next_pos(ioq->tail);
    if (ioq->producer != 0) {
        wakeup(&ioq->producer);
    }
    lock_release(&ioq->lock);
    return byte;
}
