#ifndef USERPROG_PE_H
#define USERPROG_PE_H

#include "kernel/userprog/exec.h"
#include <stdint.h>

#define PE_DOS_MAGIC 0x5a4du
#define PE_SIG 0x00004550u
#define PE_MACHINE_AMD64 0x8664u
#define PE_MAGIC_PE32P 0x20bu
#define PE_SECTION_MAX 24
#define PE_IMPORT_MAX 32
#define PE_RELOC_PAGE_MAX 256
#define PE_RELOC_DIR64 10u
#define PE_FALLBACK_BASE 0x10000000u
#define PE_NAME_MAX 96

#define PE_SCN_CNT_CODE 0x00000020u
#define PE_SCN_MEM_EXECUTE 0x20000000u

struct PE_SECTION {
    uint32_t va;
    uint32_t vsize;
    uint32_t raw_off;
    uint32_t raw_size;
    uint32_t flags;
};

struct PE_IMAGE {
    uint64_t image_base;
    uint32_t base;
    uint32_t entry;
    uint32_t app_entry;
    uint32_t size_image;
    uint32_t size_headers;
    uint32_t import_rva;
    uint32_t import_size;
    uint32_t reloc_rva;
    uint32_t reloc_size;
    uint32_t image_end;
    uint32_t thunk_base;
    uint32_t rt;
    uint32_t brk_base;
    uint32_t nsec;
    struct PE_SECTION sec[PE_SECTION_MAX];
};

int32_t pe_load(int32_t fd, struct EXEC_IMAGE *img);

#endif