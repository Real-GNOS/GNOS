#include "arch/x86/interrupt/interrupt.h"
#include "drivers/block/ide.h"
#include "drivers/char/console/io.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/mouse.h"
#include "kernel/fs/file.h"
#include "kernel/shell/pipe.h"
#include "kernel/asm/stub.h"
#include "kernel/asm_func.h"
#include "kernel/init/apic/apic.h"
#include "kernel/init/pic/pic.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/sched/percpu.h"
#include "kernel/signal.h"
#include "kernel/userprog/process.h"
#include "lib/str/str.h"
volatile uint32_t tick = 0;
static const char *exc_names[32] = {"Divide Error",
                                    "Debug",
                                    "Non-Maskable Interrupt",
                                    "Breakpoint",
                                    "Overflow",
                                    "BOUND Range Exceeded",
                                    "Invalid Opcode",
                                    "Device Not Available",
                                    "Double Fault",
                                    "Coprocessor Seg Overrun",
                                    "Invalid TSS",
                                    "Segment Not Present",
                                    "Stack-Segment Fault",
                                    "General Protection",
                                    "Page Fault",
                                    "(Reserved)",
                                    "x87 FP Error",
                                    "Alignment Check",
                                    "Machine Check",
                                    "SIMD FP Exception",
                                    "Virtualization Exception",
                                    "Control Protection",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)",
                                    "(Reserved)"};
static uint32_t tlb_retry_rip;
static uint32_t tlb_retry_pid;
static int handle_cow_fault(uint32_t fault_addr, uint32_t error_code) {
    if (fault_addr < USER_VADDR_START || fault_addr >= 0xc0000000) {
        return 0;
    }
    if (!(error_code & 0x2)) {
        return 0;
    }
    if (current == 0 || current->pml4_phys == 0) {
        return 0;
    }
    uint64_t *pte = pte_ptr(fault_addr);
    if (!(*pte & 1) || !(*pte & COW_FLAG)) {
        return 0;
    }
    return page_cow_resolve(fault_addr, *pte);
}

static int handle_stack_grow(uint32_t fa, uint32_t err_code, uint32_t rsp) {
    struct TASK *cur = current;
    if (cur == NULL || cur->pml4_phys == 0 || (err_code & 1))
        return 0;
    if (fa >= USER_STACK_TOP || fa < USER_STACK_TOP - 0x800000u)
        return 0;
    if (rsp >= USER_STACK_TOP || rsp < USER_STACK_TOP - 0x800000u)
        return 0;
    if (fa < rsp && rsp - fa > 0x10000u)
        return 0;
    if (page_is_mapped(fa))
        return 0;
    uint32_t page = fa & ~0xfffu;
    if (get_a_page(page) == 0)
        return 0;
    if (page < cur->stack_bottom || cur->stack_bottom == 0)
        cur->stack_bottom = page;
    return 1;
}

void isr_handler(struct ARCH_REGS *r) {
    uint32_t n = r->int_no;
    if (n == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (handle_cow_fault((uint32_t)cr2, r->err_code)) {
            return;
        }
        if ((r->cs & 3) == 3 &&
            handle_stack_grow((uint32_t)cr2, (uint32_t)r->err_code,
                              (uint32_t)r->user_rsp)) {
            return;
        }
    }
    if ((r->cs & 3) == 3) {
        int sig = exception_to_signal((int)n);
        if (sig > 0) {
            uint64_t cr2f = 0;
            if (sig == SIGSEGV) {
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2f));
                if ((current->signal_pending | current->signal_mask) &
                    (1u << SIGSEGV)) {
                    signal_terminate(current, SIGSEGV);
                    return;
                }
                if (cr2f < 0xc0000000ull && !(r->err_code & 1) &&
                    !(r->err_code & 0x2)) {
                    uint64_t *rpte = pte_ptr((uint32_t)cr2f);
                    if ((*rpte & 1) && (*rpte & 4) &&
                        (tlb_retry_rip != (uint32_t)r->eip ||
                         tlb_retry_pid != current->pid)) {
                        tlb_retry_rip = (uint32_t)r->eip;
                        tlb_retry_pid = current->pid;
                        uint32_t nphy = user_remap_page(cr2f & ~0xfffu);
                        if (nphy != 0) {
                            uint64_t cr3v = current->pml4_phys;
                            __asm__ volatile("mov %0, %%cr3"
                                             : : "r"(cr3v) : "memory");
                            return;
                        }
                    }
                }
            }
            current->sig_fault_addr = (uint32_t)cr2f;
            current->signal_pending |= (1u << sig);
            check_pending_signals(r);
            return;
        }
        set_text_color(12);
        kprintf("\n*** UNHANDLED USER EXCEPTION ***\n");
        if (n < 32) {
            kprintf("  %s (vector %d)  eip = 0x%x\n", exc_names[n], (int)n,
                    r->eip);
        }
        signal_terminate(current, SIGSEGV);
    }
    if (n == 14 && current != NULL && current->pml4_phys != 0) {
        uint64_t cr2v;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2v));
        uint32_t fa = (uint32_t)cr2v;
        if (fa >= USER_VADDR_START && fa < 0xc0000000u &&
            (fa < 0x40000000u || fa >= 0x80200000u)) {
            set_text_color(14);
            kprintf("[pf] kernel touched unmapped user addr 0x%x, killing "
                    "pid %d (%s) eip=0x%x err=%x mapped=%d\n",
                    fa, current->pid, current->name, (uint32_t)r->eip,
                    (uint32_t)r->err_code, page_is_mapped(fa));
            signal_terminate(current, SIGSEGV);
            return;
        }
    }
    set_text_color(12);
    kprintf("\n*** EXCEPTION ***\n");
    if (n < 32) {
        kprintf("  %s (vector %d)\n", exc_names[n], (int)n);
        kprintf("  err_code = 0x%x\n", r->err_code);
    } else {
        kprintf("  Unregistered interrupt (vector %d)\n", (int)n);
    }
    kprintf("  eip = 0x%x  cs = 0x%x  eflags = 0x%x\n", r->eip, r->cs,
            r->eflags);
    kprintf("  eax = 0x%x  ebx = 0x%x  ecx = 0x%x  edx = 0x%x\n", r->eax,
            r->ebx, r->ecx, r->edx);
    kprintf("  esi = 0x%x  edi = 0x%x  ebp = 0x%x  rflags = 0x%x\n", r->esi,
            r->edi, r->ebp, r->rflags);
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("  cr2 (fault addr) = 0x%x\n", (uint32_t)cr2);

    kprintf("  efer = 0x%x (NXE=%d)  cr4 = 0x%x (PAE=%d SMEP=%d SMAP=%d)\n",
            (uint32_t)asm_rdmsr(0xC0000080u),
            (int)((asm_rdmsr(0xC0000080u) >> 11) & 1), (uint32_t)asm_read_cr4(),
            (int)((asm_read_cr4() >> 5) & 1), (int)((asm_read_cr4() >> 20) & 1),
            (int)((asm_read_cr4() >> 21) & 1));
    if (n == 14) {
        page_table_dump((uint32_t)cr2);
    }
    if (cr2 >= 0xC1000000 && cr2 < 0xC1400000) {
        kprintf("  (note: fault is inside kernel virtual pool "
                "0xC1000000..0xC1400000)\n");
    } else if (cr2 >= 0xE0000000) {
        kprintf("  (note: fault is inside VRAM region 0xE0000000+)\n");
    }

    static volatile int panicking;
    if (panicking) {
        asm_cli();
        for (;;)
            asm_hlt();
    }
    panicking = 1;

    {
        struct TASK *cur = get_current();
        if (cur && cur->stack_magic != STACK_MAGIC)
            kprintf("  task: pid=%d name=%s stack_magic=%#x CORRUPTED "
                    "(want %#x)\n",
                    cur->pid, cur->name, cur->stack_magic,
                    (uint32_t)STACK_MAGIC);
    }

    unsigned long *rbp;
    unsigned long ret_addr;
    int frame = 1;

    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    while (rbp) {
        ret_addr = *(rbp + 1);
        kprintf("[#%d] 0x%x\n", frame++, ret_addr);

        if ((unsigned long)rbp < 0x1000)
            break;
        rbp = (unsigned long *)*rbp;
    }

    asm_cli();
    for (;;) {
        asm_hlt();
    }
}

static void irq_eoi(uint32_t irq) {
    if (apic_active()) {
        lapic_eoi();
    } else {
        pic_send_eoi((uint8_t)irq);
    }
}

static volatile uint32_t cpu_ipi_ticks[NR_CPU];

void irq_handler(struct ARCH_REGS *r) {
    if (r->int_no == IPI_VECTOR_RESCHED) {
        uint32_t c = cpu_id();
        if (c < NR_CPU)
            cpu_ipi_ticks[c]++;
        lapic_eoi();
        if (current != 0 && preempt_disabled() == 0)
            schedule();
        return;
    }
    uint32_t irq = r->int_no - 32;
    if (irq == 0) {
        irq_eoi(irq);
        tick++;
        itimer_tick();
        scheduler_tick();
        thread_timer_wake();
#define SCHED_PROBE_ENABLE 0
#if SCHED_PROBE_ENABLE
        if ((tick % 300) == 0) {
            extern volatile uint32_t sc_total;
            extern uint32_t sc_last_nr[MAX_TASKS];
            extern uint32_t sc_last_a0[MAX_TASKS];
            extern uint32_t sc_last_a1[MAX_TASKS];
            extern uint32_t sc_last_ra[MAX_TASKS];
            extern uint32_t sc_last_stk[MAX_TASKS][16];
            static uint32_t frz_sc;
            static int frz_n;
            kprintf("[rip] t=%u cur_pid=%d cs=%d rip=%x off=%x sc=%u\n",
                    (unsigned)tick, current != 0 ? (int)current->pid : -1,
                    (int)(r->cs & 3), (uint32_t)r->eip,
                    current != 0
                        ? (uint32_t)(r->eip - current->exe_bias)
                        : (uint32_t)r->eip,
                    (unsigned)sc_total);
            if (sc_total == frz_sc)
                frz_n++;
            else
                frz_n = 0;
            frz_sc = sc_total;
            if (frz_n == 2) {
                uint32_t lf = thread_all_lock();
                struct LIST_ELEM *le = thread_all_list.head.next;
                while (le != &thread_all_list.tail) {
                    struct TASK *t =
                        list_entry(le, struct TASK, all_list_tag);
                    uint32_t sl = (uint32_t)(t - task_table);
                    if (t->status != TASK_DIED)
                        kprintf("[task] pid=%d st=%x nm=%s pp=%d nr=%u "
                                "a0=%x a1=%x fu=%x/%x ft=%u si=%u ex=%d "
                                "pml4=%x path=%s\n",
                                (int)t->pid, (unsigned)t->status, t->name,
                                (int)t->parent_pid,
                                (unsigned)sc_last_nr[sl],
                                (unsigned)sc_last_a0[sl],
                                (unsigned)sc_last_a1[sl],
                                (unsigned)t->futex_uaddr,
                                (unsigned)t->futex_pml4,
                                (unsigned)t->futex_timed,
                                (unsigned)t->sleep_intr,
                                (int)t->exit_status,
                                (unsigned)t->pml4_phys, t->exe_path);
                    if (t->pid >= 6)
                        sched_dbg_task(t);
                    if (t->pid >= 6) {
                        for (uint32_t fdl = 3; fdl < MAX_FILES_OPEN_PER_PROC;
                             fdl++) {
                            uint32_t g = t->fd_table[fdl];
                            uint32_t rc = 0, nb = 0, ln = 0, hd = 0, tl = 0;
                            uint32_t pr = 0, co = 0, ispipe = 0, raw = 0;
                            uint32_t peer = 0, palive = 0, isrd = 0;
                            struct FILE *fl;
                            if (g == (uint32_t)-1)
                                continue;
                            fl = file_get(g);
                            if (fl != NULL) {
                                rc = fl->ref_cnt;
                                nb = fl->fd_nonblock;
                                raw = fl->fd_flag;
                                if ((fl->fd_flag == PIPE_FLAG ||
                                     fl->fd_flag == PIPE_RD_FLAG) &&
                                    fl->fd_inode != NULL) {
                                    struct TTY_IOQUEUE *q =
                                        (struct TTY_IOQUEUE *)fl->fd_inode;
                                    ispipe = 1;
                                    isrd = (fl->fd_flag == PIPE_RD_FLAG);
                                    peer = fl->proc_aux;
                                    palive = (uint32_t)pipe_end_alive(peer);
                                    ln = ioq_length(q);
                                    hd = (uint32_t)q->head;
                                    tl = (uint32_t)q->tail;
                                    pr = (uint32_t)(uintptr_t)q->producer;
                                    co = (uint32_t)(uintptr_t)q->consumer;
                                }
                            }
                            kprintf("[fd] pid=%d l=%u g=%u fl=%x rc=%u nb=%u "
                                    "pipe=%u ln=%u h=%u t=%u pr=%x co=%x "
                                    "rd=%u peer=%u palive=%u\n",
                                    (int)t->pid, (unsigned)fdl, (unsigned)g,
                                    (unsigned)raw, (unsigned)rc, (unsigned)nb,
                                    (unsigned)ispipe, (unsigned)ln,
                                    (unsigned)hd, (unsigned)tl, (unsigned)pr,
                                    (unsigned)co, (unsigned)isrd,
                                    (unsigned)peer, (unsigned)palive);
                        }
                    }
                    if (t->self_kstack != 0 && t->pid >= 6) {
                        uint64_t *ks = t->self_kstack;
                        for (uint32_t q = 0; q < 40; q++)
                            kprintf("[kstk] pid=%d +%x=%x\n", (int)t->pid,
                                    (unsigned)(q * 8), (uint32_t)ks[q]);
                    }
                    if (t->kernel_stack_top != 0 && t->pid >= 6) {
                        struct ARCH_REGS *fr =
                            (struct ARCH_REGS *)(uintptr_t)(
                                t->kernel_stack_top -
                                sizeof(struct ARCH_REGS));
                        kprintf("[uregs] pid=%d rip=%x ursp=%x cs=%x fl=%x "
                                "int=%x err=%x rax=%x rdi=%x rsi=%x rdx=%x "
                                "cr2=%x\n",
                                (int)t->pid, (uint32_t)fr->rip,
                                (uint32_t)fr->user_rsp, (uint32_t)fr->cs,
                                (uint32_t)fr->rflags, (uint32_t)fr->int_no,
                                (uint32_t)fr->err_code, (uint32_t)fr->rax,
                                (uint32_t)fr->rdi, (uint32_t)fr->rsi,
                                (uint32_t)fr->rdx,
                                (uint32_t)asm_read_cr2());
                    }
                    if (t->pid >= 6) {
                        kprintf("[bias] pid=%d exe=%x\n", (int)t->pid,
                                (unsigned)t->exe_bias);
                        for (uint32_t k = 0; k < 8; k++)
                            kprintf("[ustk] pid=%d +%x=%x\n", (int)t->pid,
                                    (unsigned)(k * 4),
                                    (unsigned)sc_last_stk[sl][k]);
                    }
                    le = le->next;
                }
                thread_all_unlock(lf);
            }
        }
#endif
        // if (cpu_ipi_ticks[1] != 0 && (tick % 500) == 0) {
        //     kprintf("[smp] ipi1=%u ipi2=%u ipi3=%u | wrk0=%u wrk1=%u wrk2=%u "
        //             "wrk3=%u\n",
        //             (unsigned)cpu_ipi_ticks[1], (unsigned)cpu_ipi_ticks[2],
        //             (unsigned)cpu_ipi_ticks[3],
        //             (unsigned)cpu_work_switches[0],
        //             (unsigned)cpu_work_switches[1],
        //             (unsigned)cpu_work_switches[2],
        //             (unsigned)cpu_work_switches[3]);
        // }
        if (apic_active())
            lapic_send_ipi_all_but_self(IPI_VECTOR_RESCHED);
        if (current != 0) {
            check_pending_signals(r);
            if (preempt_disabled() == 0)
                schedule();
        }
        return;
    }
    if (irq == 1) {
        keyboard_handler();
    }
    if (irq == 12) {
        mouse_handler();
    }
    if (irq == 14) {
        intr_hd_handler((uint8_t)r->int_no);
    }
    irq_eoi(irq);
}
