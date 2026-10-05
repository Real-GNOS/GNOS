#ifndef SCHED_THREAD_H
#define SCHED_THREAD_H

#include "kernel/mm/pool/pool.h"
#include "kernel/sched/percpu.h"
#include "kernel/signal.h"
#include "lib/list/list.h"
#include <stdint.h>

#define THREAD_STACK_SIZE 0x8000
#define MAX_TASKS 64
#define STACK_MAGIC 0x19860726
#define FPU_SAVE_SIZE 512

#define RFLAGS_INIT 0x202u
#define MAX_FILES_OPEN_PER_PROC 64
typedef int32_t pid_t;
enum TASK_STATUS {
    TASK_RUNNING = 1u << 0,
    TASK_READY = 1u << 1,
    TASK_BLOCKED = 1u << 2,
    TASK_WAITING = 1u << 3,
    TASK_HANGING = 1u << 4,
    TASK_DIED = 1u << 5,
    TASK_STOPPED = 1u << 6
};
#define TASK_WAKE_MASK (TASK_BLOCKED | TASK_WAITING | TASK_HANGING)
#define TASK_DEAD_MASK (TASK_DIED | TASK_HANGING)
typedef void (*thread_func)(void *);

struct TASK_STACK {
    uint64_t rflags;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    void (*rip)(void);
};
struct TASK {
    uint64_t *self_kstack;
    enum TASK_STATUS status;
    uint32_t pid;
    char name[16];
    uint8_t priority;
    uint32_t elapsed_ticks;
    uint64_t vruntime;
    uint64_t deadline;
    uint32_t weight;
    uint32_t slice;
    struct LIST_ELEM all_list_tag;
    struct LIST_ELEM futex_tag;
    struct LIST_ELEM wait_tag;
    uint32_t futex_ready;
    uint32_t futex_uaddr;
    uint32_t futex_pml4;
    uint32_t futex_bitset;
    uint32_t futex_timed;
    uint32_t sleep_intr;
    uint32_t sleep_eintr;
    uint32_t sleep_left;
    int32_t parent_pid;
    int32_t exit_status;
    uint64_t kernel_stack_top;
    uint32_t pml4_phys;
    struct MM_VADDR userprog_v_addr;
    uint32_t user_brk;
    uint32_t brk_base;
    uint32_t stack_bottom;
    uint32_t signal_pending;
    uint32_t signal_mask;
    struct SYS_SIGACTION sigactions[NSIG];
    uint32_t cwd_inode_nr;
    uint32_t fd_table[MAX_FILES_OPEN_PER_PROC];
    uint64_t pipe_wr_mask;
    uint32_t tls_base;
    uint32_t tls_selector;
    uint8_t tls_msr;
    int32_t errno;
    uint32_t pgid;
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;
    uint32_t umask;
    uint32_t sid;
    uint64_t itimer_expire;
    uint64_t itimer_interval;
    uint64_t sigalt_sp;
    uint32_t sigalt_size;
    uint32_t sigalt_flags;
    uint32_t compat;
    uint32_t clear_child_tid;
    char exe_path[256];
    uint32_t exe_bias;
    uint32_t win_base;
    uint32_t win_rt;
    uint32_t sig_fault_addr;
    uint32_t stack_magic;
    uint64_t fd_cloexec;
    int32_t fd_owner_pid;
    uint8_t slot_used;
    uint8_t cpu_aff;
    uint32_t on_cpu;
    uint8_t fpu_storage[FPU_SAVE_SIZE] __attribute__((aligned(64)));
};
extern struct TASK task_table[MAX_TASKS];
extern struct TASK *idle_threads[NR_CPU];
extern uint32_t cpu_work_switches[NR_CPU];
extern struct LIST thread_all_list;
extern uint32_t foreground_pid;
uint32_t thread_all_lock(void);
void thread_all_unlock(uint32_t flags);
void thread_init(void);
void cpu_idle_init(void);
void cpu_idle(void);
void kernel_thread(char *name, uint8_t priority, thread_func function,
                   void *arg, uint8_t aff);
struct TASK *thread_create(char *name, uint8_t priority,
                                  thread_func function, void *arg,
                                  uint8_t aff);
void schedule(void);
void scheduler_tick(void);
void switch_to(uint64_t **cur_kstack, uint64_t **next_kstack, void *fp_save,
               void *fp_restore);
void kernel_thread_entry(void);
void thread_block(void);
void thread_unblock(struct TASK *t);
int32_t thread_sleep_ticks(uint32_t ticks);
void thread_timer_wake(void);
void sched_dbg_task(struct TASK *t);
struct TASK *fd_owner_task(void);
void thread_yield(void);
void thread_block_with_status(enum TASK_STATUS status);
uint32_t thread_block_prepare_timed(enum TASK_STATUS status, uint32_t ticks);
void thread_timer_cancel(void);
void thread_timer_disarm(struct TASK *t);
uint32_t thread_block_prepare(enum TASK_STATUS status);
void thread_block_commit(uint32_t flags);
struct TASK *pid2thread(int32_t pid);
void thread_exit(struct TASK *thread_over, int need_schedule);
typedef int (*thread_all_action)(struct TASK *, void *);
int thread_traverse_all(thread_all_action action, void *arg);
struct TASK *thread_alloc_slot(const char *name, uint8_t priority);
void thread_ready(struct TASK *t);
void thread_exit_current(void);
void preempt_disable(void);
void preempt_enable(void);
uint32_t preempt_disabled(void);
void thread_kill_pid(uint32_t pid);
#endif /* SCHED_THREAD_H */
