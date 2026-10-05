#ifndef ARCH_X86_UEFI_LOADER_H
#define ARCH_X86_UEFI_LOADER_H

#include <stdint.h>

#include "arch/x86/uefi/efi.h"
#include "kernel/init/mb2.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

void *memcpy(void *dst, const void *src, uint64_t len);
void *memset(void *dst, int value, uint64_t len);

#define EFI_MBI_PHYS 0x5000ull
#define EFI_MBI_PAGES 1ull
#define EFI_LANDING_PHYS 0x6000ull
#define EFI_LANDING_PAGES 1ull
#define EFI_STACK_BASE_PHYS 0x7000ull
#define EFI_STACK_TOP_PHYS 0x87000ull
#define EFI_STACK_PAGES 128ull

#define EFI_PAGE_TABLE_PHYS 0x90000ull
#define EFI_PAGE_TABLE_ALLOC_PAGES 16ull
#define EFI_PML4_PHYS 0x90000ull
#define EFI_PDPT_PHYS 0x91000ull
#define EFI_PD_LO_PHYS 0x92000ull
#define EFI_PD_VRAM_PHYS 0x94000ull
#define EFI_PD_MMIO_PHYS 0x96000ull
#define EFI_PD_HI_PHYS 0x98000ull

#define EFI_KERNEL_PHYS 0x280000ull
#define EFI_KERNEL_BASE_PHYS 0x200000ull
#define EFI_BOOT_MAGIC 0x36D76289u

#define EFI_KASLR_MIN 0x400000ull
#define EFI_KASLR_MAX 0x20000000ull

#define EFI_PTE_TABLE_RW 0x07ull
#define EFI_PTE_KERN_RW 0x83ull
#define EFI_PAGE_2M 0x200000ull
#define EFI_PD_ENTRIES 512ull

#define EFI_IOAPIC_PHYS 0xFEC00000ull
#define EFI_LAPIC_PHYS 0xFEE00000ull

#define EFI_VRAM_2M_MASK 0xFFE00000ull
#define EFI_VRAM_PAGES 8ull

#define EFI_MMAP_BUFFER_SIZE 65536ull
#define EFI_MBI_MAX_BYTES 4096ull
#define EFI_MAP_LIMIT 0x20000000ull

#define EFI_ELF_HEADER_BYTES 8192ull
#define EFI_ELF_PHDR_MAX 8ull

#define EFI_ELFMAG0 0x7fu
#define EFI_ELFMAG1 'E'
#define EFI_ELFMAG2 'L'
#define EFI_ELFMAG3 'F'
#define EFI_ELFCLASS64 2u
#define EFI_ELFDATA2LSB 1u
#define EFI_ET_EXEC 2u
#define EFI_EM_X86_64 62u
#define EFI_PT_LOAD 1u

#define EFI_GOP_PIXEL_RGB 0u
#define EFI_GOP_PIXEL_BGR 1u

struct EFI_ELF64_EHDR {
    uint8_t e_ident[16];
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

struct EFI_ELF64_PHDR {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct EFI_FB_INFO {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint8_t red_pos;
    uint8_t red_size;
    uint8_t green_pos;
    uint8_t green_size;
    uint8_t blue_pos;
    uint8_t blue_size;
};

struct EFI_MMAP_INFO {
    const uint8_t *buf;
    uint64_t size;
    uint64_t desc_size;
};

struct EFI_RSDP_INFO {
    const uint8_t *ptr;
    uint32_t len;
};

struct EFI_LANDING_PARAMS {
    uint64_t pml4_phys;
    uint64_t entry_va;
    uint64_t stack_top;
    uint32_t mbi_phys;
    uint32_t kphys;
    uint32_t magic;
    uint32_t pad;
};

typedef void(EFIAPI *efi_landing_fn)(struct EFI_LANDING_PARAMS *params);

extern char efi_landing_start[];
extern char efi_landing_end[];

uint64_t efi_rdtsc(void);

#endif /* ARCH_X86_UEFI_LOADER_H */