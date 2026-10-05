#include "kernel/syscall/futex.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/rtc.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "kernel/init/pit/pit.h"
#include "kernel/sched/sync.h"
#include "kernel/sched/thread.h"
#include "kernel/syscall/linux_abi.h"
#include "lib/list/list.h"
#include "lib/str/str.h"

#define FUTEX_BUCKETS 64
#define FUTEX_OP_MASK 0x7f
#define FUTEX_NS_PER_SEC 1000000000LL
#define FUTEX_TICK_NS (FUTEX_NS_PER_SEC / PIT_HZ)
#define FUTEX_MAX_TICKS 0x7fffffffU
#define FUTEX_MAX_DELTA_SEC 100000000LL
#define FUTEX_READY_WAKE 1
#define FUTEX_READY_TIMEOUT 2

struct SYS_FUTEX_BUCKET {
    struct LIST waiters;
    struct SCHED_SPINLOCK lock;
};

static struct SYS_FUTEX_BUCKET futex_buckets[FUTEX_BUCKETS];
static int futex_inited = 0;

void futex_init(void) {
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        list_init(&futex_buckets[i].waiters);
        spinlock_init(&futex_buckets[i].lock);
    }
    futex_inited = 1;
}

static struct SYS_FUTEX_BUCKET *futex_bucket_for(uint32_t uaddr, uint32_t pml4_phys) {
    if (!futex_inited) {
        futex_init();
    }
    uint32_t h = (pml4_phys ^ (uaddr >> 2)) % FUTEX_BUCKETS;
    return &futex_buckets[h];
}

static int32_t futex_parse_timeout(const struct LINUX_TIMESPEC *ts, int absolute,
                                   int realtime, uint32_t *out) {
    int64_t sec = ts->tv_sec;
    int64_t nsec = ts->tv_nsec;
    if (nsec < 0 || nsec >= FUTEX_NS_PER_SEC || sec < 0) {
        return -EINVAL;
    }
    if (sec > FUTEX_MAX_DELTA_SEC) {
        sec = FUTEX_MAX_DELTA_SEC;
    }
    int64_t ns;
    if (!absolute) {
        ns = sec * FUTEX_NS_PER_SEC + nsec;
    } else {
        int64_t now_sec = realtime ? (int64_t)rtc_unix_time()
                                   : (int64_t)(tick / (uint32_t)PIT_HZ);
        int64_t now_ns =
            realtime ? 0 : (int64_t)(tick % (uint32_t)PIT_HZ) * FUTEX_TICK_NS;
        ns = (sec - now_sec) * FUTEX_NS_PER_SEC + (nsec - now_ns);
    }
    if (ns <= 0) {
        *out = 0;
        return 0;
    }
    uint64_t t = (uint64_t)ns / (uint64_t)FUTEX_TICK_NS;
    if (t == 0) {
        t = 1;
    }
    if (t > FUTEX_MAX_TICKS) {
        t = FUTEX_MAX_TICKS;
    }
    *out = (uint32_t)t;
    return 0;
}

static int32_t futex_do_wait(uint32_t uaddr, uint32_t val, uint32_t bitset,
                             int has_timeout, uint32_t ticks) {
    struct SYS_FUTEX_BUCKET *b = futex_bucket_for(uaddr, current->pml4_phys);
    current->futex_ready = 0;
    current->futex_uaddr = uaddr;
    current->futex_pml4 = current->pml4_phys;
    current->futex_bitset = bitset;
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&b->lock);
    if (*(volatile uint32_t *)(uintptr_t)uaddr != val) {
        spinlock_release(&b->lock);
        asm_restore_eflags(old);
        return -EAGAIN;
    }
    if (has_timeout && ticks == 0) {
        spinlock_release(&b->lock);
        asm_restore_eflags(old);
        return -ETIMEDOUT;
    }
    list_append(&b->waiters, &current->futex_tag);
    current->sleep_eintr = 0;
    current->sleep_intr = 1;
    uint32_t bf = thread_block_prepare_timed(TASK_BLOCKED, has_timeout ? ticks : 0);
    spinlock_release(&b->lock);
    thread_block_commit(bf);
    current->sleep_intr = 0;
    int32_t ready = (int32_t)current->futex_ready;
    current->futex_ready = 0;
    spinlock_acquire(&b->lock);
    if (elem_find(&b->waiters, &current->futex_tag)) {
        list_remove(&current->futex_tag);
    }
    spinlock_release(&b->lock);
    thread_timer_cancel();
    int32_t ret = 0;
    if (ready == FUTEX_READY_TIMEOUT) {
        ret = -ETIMEDOUT;
    } else if (current->sleep_eintr) {
        current->sleep_eintr = 0;
        ret = -EINTR;
    }
    asm_restore_eflags(old);
    return ret;
}

static int32_t futex_do_wake(uint32_t uaddr, uint32_t nr, uint32_t bitset) {
    struct SYS_FUTEX_BUCKET *b = futex_bucket_for(uaddr, current->pml4_phys);
    int32_t woken = 0;
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&b->lock);

    struct LIST_ELEM *e = b->waiters.head.next;
    while (woken < (int32_t)nr && e != &b->waiters.tail) {
        struct LIST_ELEM *next = e->next;
        struct TASK *t = list_entry(e, struct TASK, futex_tag);
        if (t->futex_uaddr == uaddr &&
            t->futex_pml4 == current->pml4_phys &&
            (t->futex_bitset & bitset) && (t->status & TASK_WAKE_MASK)) {
            list_remove(e);
            thread_timer_disarm(t);
            t->futex_ready = FUTEX_READY_WAKE;
            thread_unblock(t);
            woken++;
        }
        e = next;
    }
    spinlock_release(&b->lock);
    asm_restore_eflags(old);
    return woken;
}

int32_t sys_futex(uint32_t uaddr, uint32_t op, uint32_t val, uint32_t timeout,
                  uint32_t uaddr2, uint32_t val3) {
    (void)uaddr2;
    if (uaddr == 0) {
        return -EINVAL;
    }
    uint32_t cmd = op & FUTEX_OP_MASK;
    if (cmd == FUTEX_WAKE) {
        return futex_do_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY);
    }
    if (cmd == FUTEX_WAKE_BITSET) {
        if (val3 == 0) {
            return -EINVAL;
        }
        return futex_do_wake(uaddr, val, val3);
    }
    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        uint32_t bitset = (cmd == FUTEX_WAIT) ? FUTEX_BITSET_MATCH_ANY : val3;
        if (bitset == 0) {
            return -EINVAL;
        }
        int has_timeout = timeout != 0;
        uint32_t ticks = 0;
        if (has_timeout) {
            struct LINUX_TIMESPEC ts;
            memcpy(&ts, (const void *)(uintptr_t)timeout, sizeof(ts));
            int32_t rc = futex_parse_timeout(
                &ts, cmd == FUTEX_WAIT_BITSET,
                (op & FUTEX_CLOCK_REALTIME) != 0, &ticks);
            if (rc < 0) {
                return rc;
            }
        }
        return futex_do_wait(uaddr, val, bitset, has_timeout, ticks);
    }
    return -ENOSYS;
}
