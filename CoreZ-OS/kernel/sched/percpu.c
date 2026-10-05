#include "kernel/sched/percpu.h"
#include "kernel/asm_func.h"
#include "kernel/init/gdt/gdt.h"
#include "lib/str/str.h"

void percpu_init(void) {
    for (uint32_t i = 0; i < NR_CPU; i++) {
        memset((void *)(uintptr_t)(PER_CPU_BASE + i * PERCPU_BLOCK), 0, PERCPU_BLOCK);
    }
    uint16_t sel = SELECTOR_PER_CPU;
    __asm__ volatile("movw %0, %%gs" : : "r"(sel) : "memory");
    set_cpu_id(0);
    set_current((struct TASK *)0);
}