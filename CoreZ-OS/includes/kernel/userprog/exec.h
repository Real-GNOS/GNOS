#ifndef USERPROG_EXEC_H
#define USERPROG_EXEC_H

#include <stdint.h>

struct ARCH_REGS;

#define EXEC_WX_MAX 32

struct MM_WX_RANGE {
    uint32_t base;
    uint32_t pages;
};

struct EXEC_IMAGE {
    int32_t entry;
    int32_t app_entry;
    int is64;
    int is_linux;
    int is_pe;
    int has_interp;
    uint32_t phdr_vaddr;
    uint32_t phentsize;
    uint32_t phnum;
    uint32_t bias;
    uint32_t base;
    uint32_t brk_base;
    uint32_t pe_rt;
};

int32_t sys_execve(const char *path, const char *argv[], const char *envp[],
                   struct ARCH_REGS *regs);
int32_t sys_execv(const char *path, const char *argv[], struct ARCH_REGS *regs);

#endif
