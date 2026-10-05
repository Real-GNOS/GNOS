#ifndef SYNC_H
#define SYNC_H

#include "lib/list/list.h"
#include <stdint.h>

struct SCHED_SPINLOCK {
    volatile uint32_t locked;
};

struct SCHED_SEMAPHORE {
    uint8_t value;
    struct LIST waiters;

    struct SCHED_SPINLOCK lock;
};

struct SCHED_LOCK {
    struct TASK *holder;
    struct SCHED_SEMAPHORE semaphore;
    uint32_t holder_repeat_nr;
};

/**
 *  读写锁：读者之间并行，写者与一切互斥
 *
 *  @remarks
 *  写者优先——写者从进入等待直到释放都占用 gate，从而挡住后到的读者，避免写饥饿。
 *  不可重入——同一线程不得嵌套获取写锁，也不得在持写锁时获取读锁。
 *  gate 拦截新读者，rmutex 保护 readers 计数，wlock 是真正的排他锁
 *  （readers 由 0 变 1 时由首个读者代持）。三个信号量的 value 只在 0/1 之间翻转，
 *  因此复用 SCHED_SEMAPHORE 足够。
 *  @see rwlock_init
 */
struct SCHED_RWLOCK {
    struct SCHED_SEMAPHORE gate;
    struct SCHED_SEMAPHORE rmutex;
    struct SCHED_SEMAPHORE wlock;
    volatile uint32_t readers;
};

/**
 * 初始化读写锁。
 *
 * @param rw 待初始化的读写锁
 */
void rwlock_init(struct SCHED_RWLOCK *rw);

/**
 * 获取共享（读）锁。
 *
 * @param rw 读写锁
 *
 * @remarks
 * 多个读者可同时持有。已有写者持锁或正在等待时阻塞在当前线程。
 * 必须与 rwlock_read_release 在同一线程内配对
 */
void rwlock_read_acquire(struct SCHED_RWLOCK *rw);

/**
 * 释放共享（读）锁。
 *
 * @param rw 读写锁
 *
 * @remarks
 * 最后一个读者释放 wlock，从而唤醒等待中的写者
 */
void rwlock_read_release(struct SCHED_RWLOCK *rw);

/**
 * 获取排他（写）锁。
 *
 * @param rw 读写锁
 *
 * @warning
 * 不可重入：持有写锁期间不得再次获取写锁或读锁，否则自死锁
 */
void rwlock_write_acquire(struct SCHED_RWLOCK *rw);

/**
 * 释放排他（写）锁。
 *
 * @param rw 读写锁
 */
void rwlock_write_release(struct SCHED_RWLOCK *rw);

void spinlock_init(struct SCHED_SPINLOCK *s);
void spinlock_acquire(struct SCHED_SPINLOCK *s);
void spinlock_release(struct SCHED_SPINLOCK *s);

void sema_init(struct SCHED_SEMAPHORE *psema, uint8_t value);
void sema_down(struct SCHED_SEMAPHORE *psema);
void sema_up(struct SCHED_SEMAPHORE *psema);
void lock_init(struct SCHED_LOCK *plock);
void lock_acquire(struct SCHED_LOCK *plock);
void lock_release(struct SCHED_LOCK *plock);

#endif
