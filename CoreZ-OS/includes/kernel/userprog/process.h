#ifndef PROCESS_H
#define PROCESS_H

#include "kernel/sched/thread.h"
#include <stdint.h>

#define USER_VADDR_START 0x8048000
#define USER_STACK3_VADDR (0xc0000000 - 0x1000)
#define USER_HEAP_BASE 0xA0000000
#define USER_LOW_CEILING 0x40000000u
#define USER_HIGH_MMIO_END 0x80200000u
#define USER_STACK_TOP 0xc0000000u
#define USER_STACK_PAGES 16
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
#define USER_HEAP_LIMIT USER_STACK_BOTTOM
#define DEFAULT_PRIO 15

extern void intr_exit(void);

void start_process(void *arg);
void page_dir_activate(struct TASK *task);
void process_activate(struct TASK *task);
uint32_t *create_page_dir(void);
void free_user_space(struct TASK *t, uint32_t pml4_phys);
void task_release_space(struct TASK *t);
void space_ref(uint32_t pml4);
void space_detach_others(struct TASK *owner);
void create_user_vaddr_bitmap(struct TASK *user_prog);
void process_execute(char *path, char *name);

#endif
