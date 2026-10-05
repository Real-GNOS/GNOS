#ifndef KERNEL_INIT_MB2_H
#define KERNEL_INIT_MB2_H
#include <stdint.h>
#define MB2_BOOTLOADER_MAGIC 0x36D76289u
#define MB2_TYPE_END 0
#define MB2_TYPE_CMDLINE 1
#define MB2_TYPE_BOOT_LOADER_NAME 2
#define MB2_TYPE_MODULE 3
#define MB2_TYPE_BASIC_MEMINFO 4
#define MB2_TYPE_BOOTDEV 5
#define MB2_TYPE_MMAP 6
#define MB2_TYPE_VBE 7
#define MB2_TYPE_FRAMEBUFFER 8
#define MB2_TYPE_ELF_SECTIONS 9
#define MB2_TYPE_APM 10
#define MB2_TYPE_EFI32 11
#define MB2_TYPE_EFI64 12
#define MB2_TYPE_SMBIOS 13
#define MB2_TYPE_ACPI_OLD 14
#define MB2_TYPE_ACPI_NEW 15
#define MB2_TYPE_NETWORK 16
#define MB2_TYPE_EFI_MMAP 17
#define MB2_TYPE_EFI_BS 18
#define MB2_TYPE_EFI32_IH 19
#define MB2_TYPE_EFI64_IH 20
#define MB2_TYPE_LOAD_BASE 21
#define MB2_MMAP_AVAILABLE 1
#define MB2_MMAP_RESERVED 2
#define MB2_MMAP_ACPI_RECLAIM 3
#define MB2_MMAP_ACPI_NVS 4
#define MB2_MMAP_BAD 5
#define MB2_FB_TYPE_INDEXED 0
#define MB2_FB_TYPE_RGB 1
#define MB2_FB_TYPE_EGA 2
#define MB2_MAX_MMAP_ENTRIES 64
struct MB2_TAG {
    uint32_t type;
    uint32_t size;
};
struct MB2_TAG_STRING {
    uint32_t type;
    uint32_t size;
    char string[1];
};
struct MB2_TAG_BASIC_MEMINFO {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
};
struct MB2_TAG_BOOTDEV {
    uint32_t type;
    uint32_t size;
    uint32_t biosdev;
    uint32_t partition;
    uint32_t sub_partition;
};
struct MB2_MMAP_ENTRY {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};
struct MB2_TAG_MMAP {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct MB2_MMAP_ENTRY entries[1];
};
struct MB2_TAG_FRAMEBUFFER {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t color_info[6];
    uint8_t pad[2];
};
struct MB2_TAG_ACPI_OLD {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[20];
    uint8_t pad[4];
};
struct MB2_TAG_ACPI_NEW {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[36];
};
struct MB2_TAG_LOAD_BASE {
    uint32_t type;
    uint32_t size;
    uint32_t load_base_addr;
};
struct MB2_INFO {
    int valid;
    uint32_t total_size;
    const char *cmdline;
    const char *boot_loader_name;
    uint32_t mem_lower;
    uint32_t mem_upper;
    int has_bootdev;
    uint32_t biosdev;
    uint32_t partition;
    uint32_t sub_partition;
    int has_load_base;
    uint32_t load_base_addr;
    int has_mmap;
    uint32_t mmap_count;
    struct MB2_MMAP_ENTRY mmap[MB2_MAX_MMAP_ENTRIES];
    int has_framebuffer;
    struct MB2_TAG_FRAMEBUFFER framebuffer;
    int has_rsdp_old;
    uint8_t rsdp_old[20];
    int has_rsdp_new;
    uint8_t rsdp_new[36];
    int module_count;
    uint32_t tag_count;
};
void mb2_init(uint32_t magic, const void *mbi);
const struct MB2_INFO *mb2_get(void);
uint64_t mb2_mem_top(void);
uint32_t mb2_mem_upper_kb(void);
void mb2_dump(void);
#endif
