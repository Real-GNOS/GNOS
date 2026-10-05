#ifndef NT_MMAP_H
#define NT_MMAP_H

#include <stdint.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

struct SYS_MMAP_ARGS {
    uint32_t addr;
    uint32_t len;
    uint32_t prot;
    uint32_t flags;
    uint32_t fd;
    uint32_t offset;
};

uint32_t sys_mmap(const struct SYS_MMAP_ARGS *args);
uint32_t sys_mmap2(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags,
                   uint32_t fd, uint32_t offset);
int32_t sys_munmap(uint32_t addr, uint32_t len);
int32_t sys_mprotect(uint32_t addr, uint32_t len, uint32_t prot);

#endif
