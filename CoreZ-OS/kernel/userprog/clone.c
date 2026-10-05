#include "kernel/userprog/clone.h"
#include "kernel/fs/file.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/shell/pipe.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/fork.h"
#include "kernel/userprog/process.h"

extern void intr_exit(void);

static void build_clone_stack(struct TASK *child,
                              struct ARCH_REGS *parent_frame,
                              uint32_t user_stack) {
    uint32_t stack_top = (uint32_t)child->kernel_stack_top;
    struct ARCH_REGS *child_frame =
        (struct ARCH_REGS *)(stack_top - sizeof(struct ARCH_REGS));
    memcpy(child_frame, parent_frame, sizeof(struct ARCH_REGS));
    child_frame->eax = 0;
    child_frame->user_esp = user_stack;
    struct TASK_STACK *ts =
        (struct TASK_STACK *)((uint8_t *)child_frame -
                                sizeof(struct TASK_STACK));
    memset(ts, 0, sizeof(struct TASK_STACK));
    ts->rflags = RFLAGS_INIT;
    ts->rip = (void (*)(void))intr_exit;
    child->self_kstack = (uint64_t *)ts;
}

pid_t sys_clone_ex(uint32_t flags, uint32_t child_user_stack, uint32_t tls,
                   struct ARCH_REGS *r);
pid_t sys_clone(struct ARCH_REGS *r) {
    return sys_clone_ex((uint32_t)r->ebx, (uint32_t)r->ecx, (uint32_t)r->ebp, r);
}

pid_t sys_clone_ex(uint32_t flags, uint32_t child_user_stack, uint32_t tls,
                   struct ARCH_REGS *r) {
    struct TASK *parent = current;
    struct TASK *child =
        thread_alloc_slot(parent->name, parent->priority);
    if (child == NULL) {
        return -1;
    }
    child->parent_pid = (int32_t)parent->pid;
    child->cwd_inode_nr = parent->cwd_inode_nr;
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
    child->exe_bias = parent->exe_bias;
    child->user_brk = parent->user_brk;
    child->brk_base = parent->brk_base;
    child->stack_bottom = (flags & CLONE_THREAD) ? 0 : parent->stack_bottom;
    child->fd_owner_pid = (flags & CLONE_FILES)
                              ? parent->fd_owner_pid
                              : (int32_t)child->pid;
    for (uint32_t i = 0; i < MAX_FILES_OPEN_PER_PROC; i++) {
        child->fd_table[i] = parent->fd_table[i];
        if (!(flags & CLONE_FILES) && child->fd_table[i] != (uint32_t)-1 &&
            child->fd_table[i] < MAX_FILE_OPEN) {
            file_table_ref(child->fd_table[i]);
        }
    }
    child->pipe_wr_mask = parent->pipe_wr_mask;
    child->fd_cloexec = parent->fd_cloexec;
    child->exit_status = 0;
    child->signal_mask = parent->signal_mask;
    child->signal_pending = 0;
    for (int i = 0; i < NSIG; i++) {
        child->sigactions[i] = parent->sigactions[i];
    }
    child->tls_base = parent->tls_base;
    child->tls_selector = parent->tls_selector;
    child->tls_msr = parent->tls_msr;
    child->compat = parent->compat;
    child->pgid = parent->pgid ? parent->pgid : parent->pid;
    child->sid = parent->sid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->suid = parent->suid;
    child->sgid = parent->sgid;
    if (flags & CLONE_SETTLS) {
        child->tls_base = tls;
        child->tls_selector = 0;
        child->tls_msr = 1;
    }
    if (flags & CLONE_THREAD) {
        child->parent_pid = -1;
    }
    if ((flags & CLONE_CHILD_CLEARTID) != 0) {
        child->clear_child_tid = (uint32_t)r->r10;
        if ((flags & CLONE_CHILD_SETTID) != 0 && r->r10 != 0)
            *(volatile int32_t *)(uintptr_t)r->r10 = 0;
    } else if ((flags & CLONE_CHILD_SETTID) != 0) {
        child->clear_child_tid = 0;
    }

    if (flags & CLONE_VM) {
        if (parent->pml4_phys == 0) {
            goto clone_fail;
        }
        child->pml4_phys = parent->pml4_phys;
        child->userprog_v_addr = parent->userprog_v_addr;
        space_ref(child->pml4_phys);
    } else {
        create_user_vaddr_bitmap(child);
        child->pml4_phys = (uint32_t)create_page_dir();
        if (child->pml4_phys == 0) {
            goto clone_fail;
        }
        space_ref(child->pml4_phys);
        if (copy_user_space(parent, child) != 0) {
            goto clone_fail;
        }
    }

    if (child_user_stack == 0) {
        if (flags & CLONE_VM) {
            for (uint32_t v = USER_STACK_BOTTOM - PAGE_SIZE;
                 v > USER_VADDR_START; v -= PAGE_SIZE) {
                uint64_t *pde = pde_ptr(v);
                uint64_t *pte = pte_ptr(v);
                if (pde != NULL && (*pde & 0x80)) {
                    continue;
                }
                if (pte != NULL && (*pte & 1)) {
                    continue;
                }
                void *p = get_a_page(v);
                if (p != 0) {
                    child_user_stack = v + PAGE_SIZE;
                }
                break;
            }
            if (child_user_stack == 0) {
                goto clone_fail;
            }
        } else {
            child_user_stack = (uint32_t)r->user_esp;
        }
    }
    build_clone_stack(child, r, child_user_stack);
    child->status = TASK_BLOCKED;
    thread_ready(child);
    return (pid_t)child->pid;

clone_fail:
    kprintf("[clone-fail] flags=%x parent=%d\n", (unsigned)flags,
            (int)parent->pid);
    free_user_space(child, child->pml4_phys);
    for (uint32_t i = 0; i < MAX_FILES_OPEN_PER_PROC; i++) {
        uint32_t g = child->fd_table[i];
        if (g != (uint32_t)-1 && g < MAX_FILE_OPEN) {
            if (file_table[g].ref_cnt > 0) {
                file_table_unref(g);
            }
        }
    }
    thread_exit(child, 0);
    return -1;
}
