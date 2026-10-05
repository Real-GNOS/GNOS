#ifndef ARCH_X86_CPU_H
#define ARCH_X86_CPU_H

#include <stdint.h>

static inline void cpu_cli(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void cpu_sti(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline void cpu_hlt(void) {
    __asm__ volatile("hlt");
}

static inline void cpu_pause(void) {
    __asm__ volatile("pause");
}

static inline uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t cpu_eflags(void) {
    uint64_t v;
    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(v));
    return (uint32_t)v;
}

static inline void cpu_set_eflags(uint32_t v) {
    __asm__ volatile("pushq %0\n\tpopfq" :: "r"((uint64_t)v) : "memory");
}

static inline uint32_t cpu_xchg32(volatile uint32_t *addr, uint32_t v) {
    uint32_t out;
    __asm__ volatile("xchg %0, %1" : "=r"(out), "+m"(*addr) : "0"(v) : "memory");
    return out;
}

/**
 * 原子比较交换。
 *
 * @param addr   目标地址
 * @param expect 期望的旧值
 * @param newv   旧值等于 expect 时写入的新值
 * @returns addr 处的旧值；返回值等于 expect 表示写入成功
 *
 * @remarks
 * 使用 lock 前缀指令，隐含完整内存屏障
 */
static inline uint32_t cpu_cmpxchg32(volatile uint32_t *addr, uint32_t expect,
                                     uint32_t newv) {
    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "+a"(expect), "+m"(*addr)
                     : "r"(newv)
                     : "memory");
    return expect;
}

/**
 * 原子加，返回相加前的旧值。
 *
 * @param addr 目标地址
 * @param inc  增量；递减时传 (uint32_t)-1
 * @returns addr 处的旧值
 *
 * @remarks
 * 使用 lock 前缀指令，隐含完整内存屏障
 */
static inline uint32_t cpu_xadd32(volatile uint32_t *addr, uint32_t inc) {
    __asm__ volatile("lock xaddl %0, %1" : "+r"(inc), "+m"(*addr) : : "memory");
    return inc;
}

/**
 * 原子读取 32 位值。
 *
 * @param addr 目标地址
 * @returns addr 处的当前值
 *
 * @remarks
 * 对齐的 32 位读在 x86 上本就是原子的；本函数的作用是阻止编译器把值
 * 缓存在寄存器中，保证每次调用都能观察到最新写入
 */
static inline uint32_t cpu_atomic_load32(const volatile uint32_t *addr) {
    uint32_t v;
    __asm__ volatile("movl %1, %0" : "=r"(v) : "m"(*addr) : "memory");
    return v;
}

static inline void cpu_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t cpu_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t cpu_inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t cpu_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_ins(uint16_t port, void *buf, int count, int width) {
    if (width == 4)
        __asm__ volatile("rep insl" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
    else if (width == 2)
        __asm__ volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
    else
        __asm__ volatile("rep insb" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void cpu_outs(uint16_t port, const void *buf, int count, int width) {
    if (width == 4)
        __asm__ volatile("rep outsl" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
    else if (width == 2)
        __asm__ volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
    else
        __asm__ volatile("rep outsb" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

#endif /* ARCH_X86_CPU_H */