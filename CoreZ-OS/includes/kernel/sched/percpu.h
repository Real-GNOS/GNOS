#ifndef PERCPU_H
#define PERCPU_H
#include <stdint.h>
#define PER_CPU_BASE 0x00100000u
#define NR_CPU 4
#define PERCPU_BLOCK 0x1000

#define PERCPU_OFF_PREEMPT 0x0C
#define PERCPU_OFF_IDLE_MON 0x800

struct TASK;
#define SELECTOR_PER_CPU 0x40
static inline struct TASK *get_current(void) {
    struct TASK *cur;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(cur) : : "memory");
    return cur;
}

static inline void set_current(struct TASK *t) {
    __asm__ volatile("movq %0, %%gs:0" : : "r"(t) : "memory");
}

static inline uint32_t cpu_id(void) {
    uint32_t id;
    __asm__ volatile("movl %%gs:8, %0" : "=r"(id));
    return id;
}

static inline void set_cpu_id(uint32_t id) {
    __asm__ volatile("movl %0, %%gs:8" : : "r"(id) : "memory");
}

static inline uint32_t percpu_preempt_count(void) {
    uint32_t v;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(v) : "i"(PERCPU_OFF_PREEMPT) : "memory");
    return v;
}

static inline void percpu_preempt_inc(void) {
    __asm__ volatile("incl %%gs:%c0" : : "i"(PERCPU_OFF_PREEMPT) : "memory");
}

static inline void percpu_preempt_dec(void) {
    __asm__ volatile("decl %%gs:%c0" : : "i"(PERCPU_OFF_PREEMPT) : "memory");
}

static inline uint32_t *percpu_idle_monitor(uint32_t c) {
    return (uint32_t *)(uintptr_t)(PER_CPU_BASE + (c) * PERCPU_BLOCK + PERCPU_OFF_IDLE_MON);
}

#define current get_current()
void percpu_init(void);
#endif