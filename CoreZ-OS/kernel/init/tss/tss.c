#include "kernel/init/tss/tss.h"

#include "kernel/asm_func.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/sched/percpu.h"
#include "kernel/init/gdt/gdt.h"

static struct X86_TSS cpu_tss[NR_CPU] __attribute__((aligned(16)));

uint64_t syscall_kstack_top_data = 0;

struct X86_TSS *tss_cpu(uint32_t idx) {
    return &cpu_tss[idx];
}

static void tss_fill(struct X86_TSS *t, uint64_t rsp0) {
    t->reserved1 = 0;
    t->rsp0 = rsp0;
    t->rsp1 = 0;
    t->rsp2 = 0;
    t->reserved2 = 0;
    for (int i = 0; i < 7; i++) {
        t->ist[i] = 0;
    }
    t->reserved3 = 0;
    t->reserved4 = 0;
    t->iomap_base = (uint16_t)sizeof(struct X86_TSS);
}

void tss_ap_init(uint32_t idx, uint32_t kstack_top) {
    tss_fill(tss_cpu(idx), (uint64_t)kstack_top);
}

void tss_update_rsp0(struct TASK *task) {
    uint64_t top = (uint64_t)task->kernel_stack_top;
    cpu_tss[cpu_id()].rsp0 = top;
    if (task->pml4_phys != 0)
        syscall_kstack_top_data = top;
}

void tss_init(void) {
    uint32_t kstack = (uint32_t)palloc(&kernel_pool);
    tss_fill(tss_cpu(0), (uint64_t)kstack + PAGE_SIZE);
    set_tss_desc((uint64_t)tss_cpu(0), sizeof(struct X86_TSS) - 1);
    asm_ltr(SELECTOR_TSS);
}
