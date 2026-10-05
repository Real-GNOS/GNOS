#ifndef USERPROG_ELF_H
#define USERPROG_ELF_H

#include <stdint.h>

#define EM_386 3
#define EM_X86_64 62

#define ET_EXEC 2
#define ET_DYN 3

enum LINUX_SEG_TYPE {
    PT_NULL,
    PT_LOAD,
    PT_DYNAMIC,
    PT_INTERP,
    PT_NOTE,
    PT_SHLIB,
    PT_PHDR
};

#define PF_X 0x1

struct LINUX_ELF64_NHDR {
    uint32_t namesz;
    uint32_t descsz;
    uint32_t type;
};
#define NT_GNU_ABI_TAG 1

struct LINUX_ELF32_EHDR {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct LINUX_ELF32_PHDR {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct LINUX_ELF64_EHDR {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct LINUX_ELF64_PHDR {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct LINUX_ELF64_DYN {
    int64_t d_tag;
    uint64_t d_val;
};

struct LINUX_ELF64_RELA {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37

#define ELF64_R_TYPE(i) ((i) & 0xffffffffu)
#define R_X86_64_RELATIVE 8

#endif
