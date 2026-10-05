#include "arch/interrupt/interrupt.h"
#include "kernel/sched/thread.h"
#include "drivers/char/console/io.h"
#include "drivers/char/keyboard.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/sync.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/list/list.h"
#include "lib/str/str.h"

static struct SCHED_SPINLOCK sched_lock;
static struct SCHED_SPINLOCK all_lock;

static uint64_t ready_bitmap;
static uint64_t ready_cpu[NR_CPU];
static uint64_t slot_inuse;
static uint64_t sleep_bitmap;
static uint32_t wake_tick[MAX_TASKS];
static uint32_t wake_pid[MAX_TASKS];

#define W0 1024U
#define WVSTEP (W0 << 10)
#define SCHED_LATENCY 8U
static uint64_t min_vruntime;
static uint64_t run_bitmap;
static uint64_t rq_weight;
static uint64_t rq_avg;

static uint32_t prio_to_weight(uint8_t prio) {
    uint32_t p = prio ? prio : 1;
    uint32_t w = W0 / (1u + p);
    return w < 16u ? 16u : w;
}

static uint32_t weight_slice(uint32_t weight) {
    uint64_t s = (uint64_t)SCHED_LATENCY * WVSTEP / weight;
    return s ? (uint32_t)s : 1u;
}

struct TASK task_table[MAX_TASKS];
static volatile uint32_t pid_alloc = 0;
static volatile uint32_t died_pending = 0;
struct LIST thread_all_list;
struct TASK *idle_threads[NR_CPU];
uint32_t cpu_work_switches[NR_CPU];
static struct TASK *volatile cpu_away[NR_CPU];
uint32_t foreground_pid = (uint32_t)-1;
static int mwait_ok;
static uint8_t fpu_template[FPU_SAVE_SIZE] __attribute__((aligned(64)));

static void fpu_state_reset(struct TASK *t) {
    memcpy(t->fpu_storage, fpu_template, FPU_SAVE_SIZE);
}

static inline uint32_t sched_lock_irq(void) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&sched_lock);
    return old;
}

static inline void sched_unlock_irq(uint32_t old) {
    spinlock_release(&sched_lock);
    asm_restore_eflags(old);
}

static inline uint32_t all_lock_irq(void) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&all_lock);
    return old;
}

static inline void all_unlock_irq(uint32_t old) {
    spinlock_release(&all_lock);
    asm_restore_eflags(old);
}

uint32_t thread_all_lock(void) {
    return all_lock_irq();
}

void thread_all_unlock(uint32_t flags) {
    all_unlock_irq(flags);
}

static inline uint32_t task_slot(struct TASK *t) {
    return (uint32_t)(t - task_table);
}

static inline uint32_t aff_of(const struct TASK *t) {
    return t->cpu_aff ? (uint32_t)t->cpu_aff : ((1u << NR_CPU) - 1u);
}

static int is_idle_task(const struct TASK *t) {
    for (uint32_t k = 0; k < NR_CPU; k++)
        if (t == idle_threads[k])
            return 1;
    return 0;
}

static void rq_add(struct TASK *t) {
    int64_t w = (int64_t)t->weight;
    int64_t total = (int64_t)rq_weight + w;
    int64_t v = (int64_t)rq_avg +
                w * ((int64_t)t->vruntime - (int64_t)rq_avg) / total;
    rq_avg = v > 0 ? (uint64_t)v : 0;
    rq_weight = (uint64_t)total;
}

static void rq_del(struct TASK *t) {
    int64_t w = (int64_t)t->weight;
    int64_t total = (int64_t)rq_weight - w;
    if (total > 0) {
        int64_t v = (int64_t)rq_avg -
                    w * ((int64_t)t->vruntime - (int64_t)rq_avg) / total;
        rq_avg = v > 0 ? (uint64_t)v : 0;
    } else {
        rq_avg = min_vruntime;
    }
    rq_weight = (uint64_t)(total > 0 ? total : 0);
}

static void set_status(struct TASK *t, enum TASK_STATUS status) {
    uint64_t bit = 1ULL << task_slot(t);
    int was = (run_bitmap & bit) != 0;
    int now = !is_idle_task(t) &&
              (status == TASK_RUNNING || status == TASK_READY);
    t->status = status;
    if (now && !was) {
        run_bitmap |= bit;
        rq_add(t);
    } else if (!now && was) {
        run_bitmap &= ~bit;
        rq_del(t);
    }
}

static uint64_t rq_avg_now(void) {
    return rq_weight ? rq_avg : min_vruntime;
}

static void place_entity(struct TASK *t) {
    uint64_t avg = rq_avg_now();
    uint64_t lag = t->slice / 2;
    if (lag < 1)
        lag = 1;
    if (t->vruntime + lag < avg)
        t->vruntime = avg - lag;
    if (avg > 0 && t->vruntime >= avg)
        t->vruntime = avg - 1;
    t->deadline = t->vruntime + t->slice;
}

static void renew_deadline(struct TASK *t) {
    if ((int64_t)(t->vruntime - t->deadline) < 0)
        return;
    t->deadline = t->vruntime + t->slice;
}

static void ready_enqueue(struct TASK *t) {
    uint64_t bit = 1ULL << task_slot(t);
    if (ready_bitmap & bit)
        return;

    if (t->status != TASK_RUNNING) {
        t->on_cpu = NR_CPU;
        place_entity(t);
    }
    ready_bitmap |= bit;
    uint32_t aff = aff_of(t);
    for (uint32_t c = 0; c < NR_CPU; c++) {
        if (aff & (1u << c))
            ready_cpu[c] |= bit;
    }
    set_status(t, TASK_READY);
}

static void ready_remove(struct TASK *t) {
    uint64_t bit = 1ULL << task_slot(t);
    ready_bitmap &= ~bit;
    for (uint32_t c = 0; c < NR_CPU; c++)
        ready_cpu[c] &= ~bit;
}

void cpu_idle_init(void) {
    mwait_ok = asm_mwait_supported();
    if (mwait_ok)
        kprintf("[idle] Enable MONITOR/MWAIT\n");
    else
        kprintf("[idle] Enable HLT\n");
}

void cpu_idle(void) {
    if (mwait_ok)
        asm_sti_mwait((uint64_t)(uintptr_t)percpu_idle_monitor(cpu_id()));
    else
        asm_stihlt();
}

static void idle(void *arg) {
    (void)arg;
    for (;;) {
        thread_block();
        cpu_idle();
    }
}

void kernel_thread_entry_c(thread_func function, void *arg) {
    function(arg);
    thread_exit_current();
}

static void init_fd_table(struct TASK *t) {
    t->fd_table[0] = 0;
    t->fd_table[1] = 1;
    t->fd_table[2] = 2;
    t->pipe_wr_mask = 0;
    for (uint32_t fd_idx = 3; fd_idx < MAX_FILES_OPEN_PER_PROC; fd_idx++)
        t->fd_table[fd_idx] = (uint32_t)-1;
    t->cwd_inode_nr = 0;
}

static void init_task_struct_basic(struct TASK *t, int32_t parent_pid) {
    set_status(t, TASK_BLOCKED);
    t->pid = cpu_xadd32(&pid_alloc, 1);
    t->elapsed_ticks = 0;
    t->kernel_stack_top = 0;
    t->pml4_phys = 0;
    init_fd_table(t);
    t->parent_pid = parent_pid;
    t->stack_magic = STACK_MAGIC;
    t->fd_cloexec = 0;
    t->fd_owner_pid = (int32_t)t->pid;
    t->clear_child_tid = 0;
    t->exe_path[0] = 0;
    t->exe_bias = 0;
    t->sig_fault_addr = 0;
    t->tls_base = 0;
    t->tls_selector = 0;
    t->tls_msr = 0;
    t->errno = 0;
    t->pgid = 0;
    t->uid = 0;
    t->gid = 0;
    t->euid = 0;
    t->egid = 0;
    t->suid = 0;
    t->sgid = 0;
    t->umask = 0o022;
    t->sid = 0;
    t->itimer_expire = 0;
    t->itimer_interval = 0;
    t->sigalt_sp = 0;
    t->sigalt_size = 0;
    t->sigalt_flags = 0;
    t->compat = 0;
    t->cpu_aff = 1;
    t->on_cpu = NR_CPU;
    fpu_state_reset(t);
    init_signal_state(t);

    t->all_list_tag.prev = t->all_list_tag.next = NULL;
    t->futex_tag.prev = t->futex_tag.next = NULL;
    t->futex_ready = 0;
    t->futex_uaddr = 0;
    t->futex_pml4 = 0;
    t->sleep_intr = 0;
    t->sleep_eintr = 0;
    t->sleep_left = 0;
    t->userprog_v_addr.vaddr_start = 0;
    t->userprog_v_addr.vaddr_bitmap.bits = NULL;
    t->userprog_v_addr.vaddr_bitmap.btmp_bytes_len = 0;
}

static void reap_died_threads(void);

struct TASK *thread_create(char *name, uint8_t priority,
                                  thread_func function, void *arg,
                                  uint8_t aff) {
    struct TASK *t = thread_alloc_slot(name, priority);
    if (t == NULL) {
        return NULL;
    }
    t->cpu_aff = aff;
    struct TASK_STACK *ts =
        (struct TASK_STACK *)(t->kernel_stack_top -
                                sizeof(struct TASK_STACK));
    ts->rflags = RFLAGS_INIT;
    ts->r15 = (uint64_t)function;
    ts->r14 = (uint64_t)arg;
    ts->r13 = ts->r12 = ts->rbx = ts->rbp = 0;
    ts->rip = kernel_thread_entry;
    uint32_t f = sched_lock_irq();
    ready_enqueue(t);
    sched_unlock_irq(f);
    return t;
}

static void thread_create_idle(uint32_t cpu) {
    struct TASK *t = thread_create("idle", 10, idle, 0, (uint8_t)(1u << cpu));
    if (t == NULL)
        return;
    uint32_t f = sched_lock_irq();
    ready_remove(t);
    set_status(t, TASK_BLOCKED);
    idle_threads[cpu] = t;
    sched_unlock_irq(f);
}

void thread_init(void) {
    spinlock_init(&sched_lock);
    spinlock_init(&all_lock);
    cpu_idle_init();
    list_init(&thread_all_list);
    slot_inuse = 1;

    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("fninit\n\tldmxcsr (%0)\n\tfxsave (%1)" ::"r"(&mxcsr),
                     "r"(fpu_template)
                     : "memory");

    set_current(&task_table[0]);
    task_table[0].self_kstack = 0;
    task_table[0].pid = pid_alloc++;
    strcpy(task_table[0].name, "main");
    task_table[0].priority = 5;
    task_table[0].weight = prio_to_weight(5);
    task_table[0].slice = weight_slice(task_table[0].weight);
    task_table[0].vruntime = 0;
    task_table[0].deadline = task_table[0].slice;
    task_table[0].elapsed_ticks = 0;
    task_table[0].kernel_stack_top = 0;
    task_table[0].pml4_phys = 0;
    init_fd_table(&task_table[0]);
    task_table[0].parent_pid = -1;
    task_table[0].stack_magic = STACK_MAGIC;
    task_table[0].fd_cloexec = 0;
    task_table[0].tls_base = 0;
    task_table[0].tls_selector = 0;
    task_table[0].tls_msr = 0;
    task_table[0].errno = 0;
    task_table[0].compat = 0;
    task_table[0].futex_tag.prev = task_table[0].futex_tag.next = NULL;
    task_table[0].wait_tag.prev = task_table[0].wait_tag.next = NULL;
    task_table[0].futex_ready = 0;
    task_table[0].futex_uaddr = 0;
    task_table[0].futex_pml4 = 0;
    task_table[0].sleep_intr = 0;
    task_table[0].sleep_eintr = 0;
    task_table[0].sleep_left = 0;
    task_table[0].slot_used = 1;
    task_table[0].cpu_aff = 1;
    task_table[0].on_cpu = 0;
    fpu_state_reset(&task_table[0]);
    set_status(&task_table[0], TASK_RUNNING);
    list_append(&thread_all_list, &task_table[0].all_list_tag);

    for (uint32_t c = 0; c < NR_CPU; c++)
        thread_create_idle(c);
}

struct TASK *thread_alloc_slot(const char *name, uint8_t priority) {
    uint32_t f = all_lock_irq();
    uint64_t free = ~slot_inuse;
    if (free == 0) {
        all_unlock_irq(f);
        kprintf("[thread] no free task slot (MAX_TASKS=%d)\n", MAX_TASKS);
        return NULL;
    }
    uint32_t i = (uint32_t)__builtin_ctzll(free);
    struct TASK *t = &task_table[i];
    slot_inuse |= 1ULL << i;
    t->slot_used = 1;
    t->pid = (uint32_t)-1;
    set_status(t, TASK_BLOCKED);
    all_unlock_irq(f);

    uint64_t stack = (uint64_t)get_kernel_pages(THREAD_STACK_SIZE / PAGE_SIZE);
    if (stack == 0) {
        kprintf("[thread] no kernel pages for stack (free pages: %d)\n",
                (int)kernel_pool_free_count());
        uint32_t f2 = all_lock_irq();
        t->slot_used = 0;
        slot_inuse &= ~(1ULL << i);
        all_unlock_irq(f2);
        return NULL;
    }
    struct TASK_STACK *ts =
        (struct TASK_STACK *)(stack + THREAD_STACK_SIZE -
                                sizeof(struct TASK_STACK));
    ts->rflags = RFLAGS_INIT;
    ts->r15 = ts->r14 = ts->r13 = ts->r12 = ts->rbx = ts->rbp = 0;
    ts->rip = 0;
    t->self_kstack = (uint64_t *)ts;

    uint32_t f3 = all_lock_irq();
    t->wait_tag.prev = t->wait_tag.next = NULL;
    init_task_struct_basic(t, -1);
    strcpy(t->name, name);
    t->priority = priority;
    t->weight = prio_to_weight(priority);
    t->slice = weight_slice(t->weight);
    t->vruntime = min_vruntime;
    t->deadline = min_vruntime + t->slice;
    t->kernel_stack_top = stack + THREAD_STACK_SIZE;
    list_append(&thread_all_list, &t->all_list_tag);
    all_unlock_irq(f3);

    return t;
}

void thread_ready(struct TASK *t) {
    if (t == NULL)
        return;
    uint32_t f = sched_lock_irq();
    if (t->status & TASK_WAKE_MASK)
        ready_enqueue(t);
    sched_unlock_irq(f);
}

void kernel_thread(char *name, uint8_t priority, thread_func function,
                   void *arg, uint8_t aff) {
    thread_create(name, priority, function, arg, aff);
}

uint32_t thread_block_prepare(enum TASK_STATUS status) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&sched_lock);
    ready_remove(current);
    set_status(current, status);
    spinlock_release(&sched_lock);
    return old;
}

void thread_block_commit(uint32_t flags) {
    schedule();
    asm_restore_eflags(flags);
}

void thread_block_with_status(enum TASK_STATUS status) {
    thread_block_commit(thread_block_prepare(status));
}

uint32_t thread_block_prepare_timed(enum TASK_STATUS status, uint32_t ticks) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    uint32_t slot = task_slot(current);
    spinlock_acquire(&sched_lock);
    ready_remove(current);
    set_status(current, status);
    current->futex_timed = 0;
    if (ticks) {
        wake_tick[slot] = tick + ticks;
        wake_pid[slot] = current->pid;
        sleep_bitmap |= 1ULL << slot;
        current->futex_timed = 1;
    }
    spinlock_release(&sched_lock);
    return old;
}

void thread_timer_cancel(void) {
    uint32_t f = sched_lock_irq();
    sleep_bitmap &= ~(1ULL << task_slot(current));
    current->futex_timed = 0;
    sched_unlock_irq(f);
}

void thread_timer_disarm(struct TASK *t) {
    uint32_t f = sched_lock_irq();
    sleep_bitmap &= ~(1ULL << task_slot(t));
    t->futex_timed = 0;
    sched_unlock_irq(f);
}

void thread_block(void) {
    thread_block_with_status(TASK_BLOCKED);
}

void thread_unblock(struct TASK *t) {
    thread_ready(t);
}

int32_t thread_sleep_ticks(uint32_t ticks) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    uint32_t slot = task_slot(current);
    spinlock_acquire(&sched_lock);
    current->sleep_intr = 1;
    current->sleep_eintr = 0;
    current->sleep_left = 0;
    current->futex_timed = 0;
    wake_tick[slot] = tick + ticks;
    wake_pid[slot] = current->pid;
    sleep_bitmap |= 1ULL << slot;
    ready_remove(current);
    set_status(current, TASK_BLOCKED);
    spinlock_release(&sched_lock);
    schedule();
    current->sleep_intr = 0;
    int32_t ret = 0;
    if (current->sleep_eintr) {
        current->sleep_eintr = 0;
        spinlock_acquire(&sched_lock);
        sleep_bitmap &= ~(1ULL << slot);
        int32_t left = (int32_t)(wake_tick[slot] - tick);
        current->sleep_left = left > 0 ? (uint32_t)left : 0;
        spinlock_release(&sched_lock);
        ret = -EINTR;
    }
    asm_restore_eflags(old);
    return ret;
}

void thread_timer_wake(void) {
    uint32_t f = sched_lock_irq();
    uint64_t m = sleep_bitmap;
    while (m) {
        uint32_t slot = (uint32_t)__builtin_ctzll(m);
        m &= m - 1;
        if ((int32_t)(tick - wake_tick[slot]) < 0) {
            continue;
        }
        sleep_bitmap &= ~(1ULL << slot);
        struct TASK *t = &task_table[slot];
        if (t->slot_used && t->pid == wake_pid[slot] &&
            (t->status & TASK_WAKE_MASK)) {
            if (t->futex_timed) {
                t->futex_timed = 0;
                t->futex_ready = 2;
            }
            ready_enqueue(t);
        }
    }
    sched_unlock_irq(f);
}

void sched_dbg_task(struct TASK *t) {
    uint64_t bit = 1ULL << task_slot(t);
    uint64_t avg = rq_avg_now();
    kprintf("[sch] pid=%d st=%x on_cpu=%u in_ready=%d in_cpu0=%d in_run=%d "
            "vr=%x%08x dl=%x%08x w=%u sl=%u avg=%x%08x\n",
            (int)t->pid, (unsigned)t->status, (unsigned)t->on_cpu,
            (int)((ready_bitmap & bit) != 0),
            (int)((ready_cpu[0] & bit) != 0),
            (int)((run_bitmap & bit) != 0),
            (unsigned)(t->vruntime >> 32), (unsigned)t->vruntime,
            (unsigned)(t->deadline >> 32), (unsigned)t->deadline,
            (unsigned)t->weight, (unsigned)t->slice,
            (unsigned)(avg >> 32), (unsigned)avg);
}

void thread_yield(void) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    schedule();
    asm_restore_eflags(old);
}

static void assert_stack_magic(const struct TASK *t) {
    if (t->stack_magic != STACK_MAGIC) {
        kprintf("[panic] stack_magic broken: pid=%d name=%s val=%#x\n",
                t->pid, t->name, t->stack_magic);
        ASSERT(t->stack_magic == STACK_MAGIC);
    }
}

void preempt_disable(void) {
    percpu_preempt_inc();
}

void preempt_enable(void) {
    if (percpu_preempt_count() > 0)
        percpu_preempt_dec();
}

uint32_t preempt_disabled(void) {
    return percpu_preempt_count();
}

static uint32_t pick_cpu_slot(uint32_t c) {
    uint64_t avg = rq_avg_now();
    uint64_t m = ready_cpu[c];
    uint32_t best = MAX_TASKS;
    uint32_t fallback = MAX_TASKS;
    uint64_t best_vd = (uint64_t)-1;
    uint64_t fb_ve = (uint64_t)-1;
    while (m) {
        uint32_t s = (uint32_t)__builtin_ctzll(m);
        m &= m - 1;
        struct TASK *t = &task_table[s];
        if (t->on_cpu != NR_CPU && t->on_cpu != c)
            continue;
        if (t->vruntime < fb_ve) {
            fb_ve = t->vruntime;
            fallback = s;
        }
        if (t->vruntime > avg)
            continue;
        if (t->deadline < best_vd) {
            best_vd = t->deadline;
            best = s;
        }
    }
    return best != MAX_TASKS ? best : fallback;
}

void scheduler_tick(void) {
    struct TASK *cur = current;
    if (cur == NULL || cur->weight == 0)
        return;
    uint32_t f = sched_lock_irq();
    int in_rq = (run_bitmap & (1ULL << task_slot(cur))) != 0;
    if (in_rq)
        rq_del(cur);
    cur->vruntime += WVSTEP / cur->weight;
    if (in_rq)
        rq_add(cur);
    sched_unlock_irq(f);
    cur->elapsed_ticks++;
    renew_deadline(cur);
}

void schedule(void) {
    ASSERT((asm_save_eflags() & 0x200) == 0);
    assert_stack_magic(current);

    uint32_t c = cpu_id();

    struct TASK *away = cpu_away[c];
    if (away != NULL) {
        cpu_away[c] = NULL;
        if (away != current)
            cpu_cmpxchg32(&away->on_cpu, c, NR_CPU);
    }

    if (died_pending > 0)
        reap_died_threads();

    uint32_t f = sched_lock_irq();

    if (current->status == TASK_RUNNING)
        ready_enqueue(current);

    uint32_t slot = pick_cpu_slot(c);
    if (slot == MAX_TASKS && idle_threads[c] != NULL) {
        ready_enqueue(idle_threads[c]);
        slot = pick_cpu_slot(c);
    }
    if (slot == MAX_TASKS) {
        sched_unlock_irq(f);
        return;
    }
    struct TASK *next = &task_table[slot];
    ready_remove(next);
    assert_stack_magic(next);
    set_status(next, TASK_RUNNING);
    next->on_cpu = c;
    if (next != idle_threads[c])
        cpu_work_switches[c]++;

    struct TASK *prev = current;
    if (prev != next)
        cpu_away[c] = prev;
    set_current(next);
    sched_unlock_irq(f);

    process_activate(next);
    switch_to(&prev->self_kstack, &next->self_kstack, prev->fpu_storage,
              next->fpu_storage);
}

int thread_traverse_all(thread_all_action action, void *arg) {
    struct TASK *snap[MAX_TASKS];
    uint32_t n = 0;
    uint32_t f = all_lock_irq();
    struct LIST_ELEM *e = thread_all_list.head.next;
    while (e != &thread_all_list.tail && n < MAX_TASKS) {
        snap[n++] = list_entry(e, struct TASK, all_list_tag);
        e = e->next;
    }
    all_unlock_irq(f);

    for (uint32_t i = 0; i < n; i++) {
        if (action(snap[i], arg))
            return 1;
    }
    return 0;
}

void thread_exit_current(void) {
    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&sched_lock);
    set_status(current, TASK_DIED);
    ready_remove(current);
    died_pending++;
    spinlock_release(&sched_lock);
    schedule();
    asm_restore_eflags(old);
}

void thread_kill_pid(uint32_t pid) {
    struct TASK *t = NULL;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].slot_used && task_table[i].pid == pid) {
            t = &task_table[i];
            break;
        }
    }
    if (t == NULL || (t->status & TASK_DEAD_MASK))
        return;
    if (t->pml4_phys == 0)
        return;
    assert_stack_magic(t);

    uint32_t old = asm_save_eflags();
    asm_cli();
    spinlock_acquire(&sched_lock);
    t->exit_status = -1;
    set_status(t, TASK_HANGING);
    ready_remove(t);
    spinlock_release(&sched_lock);

    kill_orphan_children((int32_t)t->pid);
    list_unlink(&t->wait_tag);
    if (keyboard_ioq.consumer == t)
        keyboard_ioq.consumer = 0;
    if (keyboard_ioq.producer == t)
        keyboard_ioq.producer = 0;

    struct TASK *parent = pid2thread(t->parent_pid);
    if (parent && parent->status == TASK_WAITING)
        thread_unblock(parent);

    if (t == current)
        schedule();
    asm_restore_eflags(old);
}

struct TASK *pid2thread(int32_t pid) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].slot_used && (int32_t)task_table[i].pid == pid)
            return &task_table[i];
    }
    return NULL;
}

void thread_exit(struct TASK *thread_over, int need_schedule) {
    (void)need_schedule;
    uint32_t f = sched_lock_irq();
    if (thread_over->status == TASK_DIED) {
        sched_unlock_irq(f);
        return;
    }
    set_status(thread_over, TASK_DIED);
    ready_remove(thread_over);
    died_pending++;
    sched_unlock_irq(f);
}

static void reap_died_threads(void) {
    struct TASK *victims[8];
    uint32_t n = 0;
    uint32_t f = all_lock_irq();
    uint32_t s = sched_lock_irq();
    struct LIST_ELEM *e = thread_all_list.head.next;
    while (e != &thread_all_list.tail && n < 8) {
        struct TASK *t = list_entry(e, struct TASK, all_list_tag);
        e = e->next;
        if (t == current || t->status != TASK_DIED)
            continue;
        if (cpu_cmpxchg32(&t->on_cpu, NR_CPU, NR_CPU + 1) != NR_CPU)
            continue;
        victims[n++] = t;
    }
    sched_unlock_irq(s);
    all_unlock_irq(f);

    for (uint32_t i = 0; i < n; i++) {
        struct TASK *t = victims[i];
        assert_stack_magic(t);
        if (t->pml4_phys) {
            task_release_space(t);
        }
        if (t->kernel_stack_top) {
            uint8_t *stack_base =
                (uint8_t *)t->kernel_stack_top - THREAD_STACK_SIZE;
            for (uint32_t j = 0; j < THREAD_STACK_SIZE / PAGE_SIZE; j++)
                free_kernel_page((uint32_t)(stack_base + j * PAGE_SIZE));
            t->kernel_stack_top = 0;
        }
        uint32_t f2 = all_lock_irq();
        list_remove(&t->all_list_tag);
        t->slot_used = 0;
        all_unlock_irq(f2);
        uint32_t s2 = sched_lock_irq();
        slot_inuse &= ~(1ULL << task_slot(t));
        if (died_pending > 0)
            died_pending--;
        sched_unlock_irq(s2);
    }
}
