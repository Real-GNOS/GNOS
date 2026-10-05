#include "kernel/sched/sync.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "kernel/sched/thread.h"
void spinlock_init(struct SCHED_SPINLOCK *s) {
    s->locked = 0;
}

void spinlock_acquire(struct SCHED_SPINLOCK *s) {
    while (asm_xchg(&s->locked, 1) != 0) {
        while (s->locked != 0) {
            asm_pause();
        }
    }
    __asm__ volatile("" : : : "memory");
}

void spinlock_release(struct SCHED_SPINLOCK *s) {
    __asm__ volatile("" : : : "memory");
    s->locked = 0;
}

void sema_init(struct SCHED_SEMAPHORE *psema, uint8_t value) {
    psema->value = value;
    list_init(&psema->waiters);
    spinlock_init(&psema->lock);
}

void sema_down(struct SCHED_SEMAPHORE *psema) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&psema->lock);
    while (psema->value == 0) {
        list_append(&psema->waiters, &current->wait_tag);
        uint32_t bf = thread_block_prepare(TASK_BLOCKED);
        spinlock_release(&psema->lock);
        thread_block_commit(bf);
        spinlock_acquire(&psema->lock);
    }
    psema->value--;
    spinlock_release(&psema->lock);
    asm_restore_eflags(old);
}

void sema_up(struct SCHED_SEMAPHORE *psema) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&psema->lock);
    if (!list_empty(&psema->waiters)) {
        struct LIST_ELEM *e = list_pop_front(&psema->waiters);
        struct TASK *w = list_entry(e, struct TASK, wait_tag);
        w->wait_tag.next = 0;
        w->wait_tag.prev = 0;
        thread_unblock(w);
    }
    psema->value++;
    spinlock_release(&psema->lock);
    asm_restore_eflags(old);
}

void lock_init(struct SCHED_LOCK *plock) {
    plock->holder = 0;
    plock->holder_repeat_nr = 0;
    sema_init(&plock->semaphore, 1);
}

struct SCHED_LKLOG { uint32_t ev, task, holder, val; };
static struct SCHED_LKLOG lkbuf[64];
static uint32_t lkidx;
static void lkdump(const char *why) {
    kprintf("[lkdump] %s idx=%u\n", why, lkidx & 63);
    for (uint32_t i = 0; i < 64; i++) {
        struct SCHED_LKLOG *e = &lkbuf[(lkidx + i) & 63];
        kprintf("[ev] %u t=%x h=%x v=%u\n", e->ev, e->task, e->holder, e->val);
    }
}

static void lklog(struct SCHED_LOCK *p, uint32_t ev) {

    uint32_t h = (uint32_t)(uintptr_t)p->holder;
    uint32_t v = p->semaphore.value;
    uint32_t bad;
    if (ev == 1)
        bad = (h != 0);
    else if (ev == 2)
        bad = (v > 1) || (h != 0);
    else
        bad = (v > 1) || ((v == 0) != (h != 0));
    if (bad) {
        kprintf("[lkviol] ev=%u t=%x h=%x v=%u\n", ev,
                (uint32_t)(uintptr_t)current, h, v);
        lkdump("violation");
        for (;;)
            cpu_hlt();
    }
    struct SCHED_LKLOG *e = &lkbuf[lkidx++ & 63];
    e->ev = ev;
    e->task = (uint32_t)(uintptr_t)current;
    e->holder = h;
    e->val = v;
}

void lock_acquire(struct SCHED_LOCK *plock) {
    if (current == 0)
        return;
    uint32_t old = asm_save_eflags();
    asm_cli();
    if (plock->holder == current) {
        plock->holder_repeat_nr++;
        lklog(plock, 5);
        asm_restore_eflags(old);
        return;
    }
    spinlock_acquire(&plock->semaphore.lock);
    while (plock->semaphore.value == 0) {
        list_append(&plock->semaphore.waiters, &current->wait_tag);
        lklog(plock, 3);
        uint32_t bf = thread_block_prepare(TASK_BLOCKED);
        spinlock_release(&plock->semaphore.lock);
        thread_block_commit(bf);
        spinlock_acquire(&plock->semaphore.lock);
        lklog(plock, 4);
    }
    plock->semaphore.value--;
    lklog(plock, 1);
    if (plock->holder != 0) {
        for (uint32_t i = 0; i < 64; i++) {
            struct SCHED_LKLOG *e = &lkbuf[(lkidx + i) & 63];
            kprintf("[ev] %u t=%x h=%x v=%u\n", e->ev, e->task, e->holder, e->val);
        }
        struct TASK *h = plock->holder;
        kprintf("[lock] corrupt lock=%x holder=%x hp=%d hn=%s hs=%d cur=%x cp=%d cn=%s rpt=%d\n",
                (uint32_t)(uintptr_t)plock, (uint32_t)(uintptr_t)h,
                (int)h->pid, h->name, (int)h->status,
                (uint32_t)(uintptr_t)current,
                current ? (int)current->pid : -1,
                current ? current->name : "?",
                (int)plock->holder_repeat_nr);
    }
    ASSERT(plock->holder == 0);
    ASSERT(plock->holder_repeat_nr == 0);
    plock->holder = current;
    plock->holder_repeat_nr = 1;
    spinlock_release(&plock->semaphore.lock);
    asm_restore_eflags(old);
}

void lock_release(struct SCHED_LOCK *plock) {
    if (current == 0)
        return;
    uint32_t old = asm_save_eflags();
    asm_cli();
    ASSERT(plock->holder == current);

    if (plock->holder_repeat_nr > 1) {
        plock->holder_repeat_nr--;
        asm_restore_eflags(old);
        return;
    }

    spinlock_acquire(&plock->semaphore.lock);
    plock->holder = 0;
    plock->holder_repeat_nr = 0;
    plock->semaphore.value++;
    lklog(plock, 2);
    if (!list_empty(&plock->semaphore.waiters)) {
        struct LIST_ELEM *e = list_pop_front(&plock->semaphore.waiters);
        struct TASK *w = list_entry(e, struct TASK, wait_tag);
        w->wait_tag.next = 0;
        w->wait_tag.prev = 0;
        thread_unblock(w);
    }
    spinlock_release(&plock->semaphore.lock);
    asm_restore_eflags(old);
}

/**
 * 读写锁实现。
 *
 * @remarks
 * 不变量：
 *  1. readers > 0 时 wlock 已被首个读者持有，写者阻塞在 sema_down(wlock) 上；
 *  2. 写者自 sema_down(gate) 起直到 sema_up(gate) 为止持有 gate，故写者在等待与
 *     持锁期间新读者无法进入，实现写者优先；
 *  3. readers 由 rmutex 保护。
 * 首个读者获取 wlock 时必然成功（gate 已保证无活跃写者），因此不会在持 rmutex
 * 的窗口内阻塞。对外契约见 includes/kernel/sched/sync.h。
 */
void rwlock_init(struct SCHED_RWLOCK *rw) {
    sema_init(&rw->gate, 1);
    sema_init(&rw->rmutex, 1);
    sema_init(&rw->wlock, 1);
    rw->readers = 0;
}

void rwlock_read_acquire(struct SCHED_RWLOCK *rw) {
    if (current == 0)
        return;
    sema_down(&rw->gate);
    sema_down(&rw->rmutex);
    if (++rw->readers == 1) {
        sema_down(&rw->wlock);
    }
    sema_up(&rw->rmutex);
    sema_up(&rw->gate);
}

void rwlock_read_release(struct SCHED_RWLOCK *rw) {
    if (current == 0)
        return;
    sema_down(&rw->rmutex);
    if (--rw->readers == 0) {
        sema_up(&rw->wlock);
    }
    sema_up(&rw->rmutex);
}

void rwlock_write_acquire(struct SCHED_RWLOCK *rw) {
    if (current == 0)
        return;
    sema_down(&rw->gate);
    sema_down(&rw->wlock);
}

void rwlock_write_release(struct SCHED_RWLOCK *rw) {
    if (current == 0)
        return;
    sema_up(&rw->wlock);
    sema_up(&rw->gate);
}
