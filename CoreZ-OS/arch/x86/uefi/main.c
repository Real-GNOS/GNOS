#include "arch/x86/uefi/loader.h"

#include <stdint.h>

#define UEFI_MEM_LOWER_LIMIT 0x100000ull
#define UEFI_MMAP_PASSES 2u
#define UEFI_EXIT_TRIES 4u

static const char uefi_cmdline[] = "corez";
static const char uefi_loader_name[] = "corez-uefi";
static CHAR16 uefi_kernel_path[] = {'\\', 'k', 'e', 'r', 'n', 'e', 'l',
                                    '.',  'e', 'l', 'f', 0};

static const struct EFI_GUID uefi_guid_loaded_image = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const struct EFI_GUID uefi_guid_simple_fs = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static const struct EFI_GUID uefi_guid_gop = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const struct EFI_GUID uefi_guid_acpi = EFI_ACPI_TABLE_GUID;
static const struct EFI_GUID uefi_guid_acpi20 = EFI_ACPI_20_TABLE_GUID;

static struct EFI_SYSTEM_TABLE *uefi_st;
static struct EFI_BOOT_SERVICES *uefi_bs;
static EFI_HANDLE uefi_image;
static struct EFI_FB_INFO uefi_fb;
static struct EFI_MMAP_INFO uefi_mmap;
static struct EFI_ELF64_PHDR uefi_phdrs[EFI_ELF_PHDR_MAX];
static struct MB2_MMAP_ENTRY uefi_mmap_merged[MB2_MAX_MMAP_ENTRIES];
static struct EFI_LANDING_PARAMS uefi_landing;
static uint8_t uefi_elf_hdr[EFI_ELF_HEADER_BYTES];
static uint8_t *uefi_mmap_buf;
static uint8_t *uefi_mb2_cursor;
static uint8_t *uefi_mb2_limit;
static const uint8_t *uefi_rsdp;
static uint32_t uefi_rsdp_len;
static uint32_t uefi_phnum;
static uint32_t uefi_mmap_count;
static uint32_t uefi_mem_lower_kb;
static uint32_t uefi_mem_upper_kb;
static uint32_t uefi_kphys;
static uint64_t uefi_img_hi;
static CHAR16 uefi_text_line[128];

static void print(const char *s) {
    uint32_t n = 0;

    if (uefi_st == NULL || uefi_st->con_out == NULL)
        return;
    while (*s != '\0') {
        uefi_text_line[n++] = (CHAR16)(uint8_t)*s++;
        if (n == 127 || *s == '\0') {
            uefi_text_line[n] = 0;
            uefi_st->con_out->output_string(uefi_st->con_out, uefi_text_line);
            n = 0;
        }
    }
}

/**
 *  以固定宽度十六进制输出
 *
 *  @param v      被输出的值
 *  @param digits 输出位数，上限 16
 */
static void print_hex(uint64_t v, uint32_t digits) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    uint32_t i;

    if (digits > 16)
        digits = 16;
    for (i = 0; i < digits; i++)
        buf[i] = hex[(v >> ((digits - 1u - i) * 4u)) & 0xFull];
    buf[digits] = '\0';
    print(buf);
}

/**
 *  以十进制输出
 *
 *  @param v 被输出的值
 */
static void print_dec(uint32_t v) {
    char buf[12];
    uint32_t i = 11;

    buf[i] = '\0';
    if (v == 0)
        buf[--i] = '0';
    while (v > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    print(&buf[i]);
}

/**
 *  输出一条引导错误
 *
 *  @param reason 失败阶段，用于在串口日志里定位
 */
static void fail(const char *reason) {
    print("EFI-ERROR ");
    print(reason);
    print("\r\n");
}

/**
 *  比较两个 GUID
 *
 *  @param a 左操作数
 *  @param b 右操作数
 *  @returns 相同返回 1，不同返回 0
 */
static int guid_eq(const struct EFI_GUID *a, const struct EFI_GUID *b) {
    int i;

    if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3)
        return 0;
    for (i = 0; i < 8; i++) {
        if (a->data4[i] != b->data4[i])
            return 0;
    }
    return 1;
}

static void find_rsdp(void) {
    uint64_t n = uefi_st->number_of_table_entries;
    uint64_t i;

    for (i = 0; i < n; i++) {
        const struct EFI_CONFIGURATION_TABLE *ct = &uefi_st->configuration_table[i];
        if (guid_eq(&ct->vendor_guid, &uefi_guid_acpi20)) {
            uefi_rsdp = (const uint8_t *)ct->vendor_table;
            uefi_rsdp_len = 36;
            return;
        }
        if (guid_eq(&ct->vendor_guid, &uefi_guid_acpi)) {
            uefi_rsdp = (const uint8_t *)ct->vendor_table;
            uefi_rsdp_len = 20;
        }
    }
}

/**
 *  在指定物理地址固定分配若干页
 *
 *  @param addr  期望的物理地址，必须页对齐
 *  @param pages 页数
 *  @param what  失败时输出的阶段名
 *  @returns 成功返回 EFI_SUCCESS；地址被占用或固件返回别的地址时输出错误并
 *           返回 EFI_LOAD_ERROR
 */
static EFI_STATUS reserve(uint64_t addr, uint64_t pages, const char *what) {
    void *got = (void *)(uintptr_t)addr;
    EFI_STATUS status;

    status = uefi_bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                     pages, &got);
    if (status != EFI_SUCCESS || (uint64_t)(uintptr_t)got != addr) {
        print("EFI-ERROR reserve ");
        print(what);
        print("\r\n");
        return EFI_LOAD_ERROR;
    }
    return EFI_SUCCESS;
}

/**
 *  打开 ESP 卷上的 \kernel.elf 并解析其程序头
 *
 *  @param file_out  输出，打开的文件句柄，调用者负责关闭
 *  @param entry_va  输出，ELF 入口虚拟地址
 *  @param img_lo    输出，首个 PT_LOAD 的物理地址（等于链接期 0x280000）
 *  @param img_hi    输出，所有 PT_LOAD 的物理末端
 *  @returns 成功返回 EFI_SUCCESS；任一步失败输出错误并返回 EFI_LOAD_ERROR
 */
static EFI_STATUS kernel_parse(struct EFI_FILE_PROTOCOL **file_out,
                               uint64_t *entry_va, uint64_t *img_lo,
                               uint64_t *img_hi) {
    struct EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    struct EFI_FILE_PROTOCOL *root = NULL;
    struct EFI_ELF64_EHDR *eh;
    uint64_t got = EFI_ELF_HEADER_BYTES;
    uint16_t i;
    EFI_STATUS status;

    status = uefi_bs->handle_protocol(uefi_image, &uefi_guid_loaded_image,
                                      (void **)&li);
    if (status != EFI_SUCCESS || li == NULL) {
        fail("loaded-image");
        return EFI_LOAD_ERROR;
    }
    status = uefi_bs->handle_protocol(li->device_handle, &uefi_guid_simple_fs,
                                      (void **)&fs);
    if (status != EFI_SUCCESS || fs == NULL) {
        fail("simple-fs");
        return EFI_LOAD_ERROR;
    }
    status = fs->open_volume(fs, &root);
    if (status != EFI_SUCCESS || root == NULL) {
        fail("open-volume");
        return EFI_LOAD_ERROR;
    }
    status = root->open(root, file_out, uefi_kernel_path, EFI_FILE_MODE_READ, 0);
    root->close(root);
    if (status != EFI_SUCCESS || *file_out == NULL) {
        fail("kernel.elf");
        return EFI_LOAD_ERROR;
    }

    status = (*file_out)->read(*file_out, &got, uefi_elf_hdr);
    if (status != EFI_SUCCESS || got < sizeof(struct EFI_ELF64_EHDR)) {
        fail("elf-header");
        return EFI_LOAD_ERROR;
    }
    eh = (struct EFI_ELF64_EHDR *)uefi_elf_hdr;
    if (eh->e_ident[0] != EFI_ELFMAG0 || eh->e_ident[1] != EFI_ELFMAG1 ||
        eh->e_ident[2] != EFI_ELFMAG2 || eh->e_ident[3] != EFI_ELFMAG3) {
        fail("elf-magic");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_ident[4] != EFI_ELFCLASS64 || eh->e_ident[5] != EFI_ELFDATA2LSB) {
        fail("elf-class");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_type != EFI_ET_EXEC || eh->e_machine != EFI_EM_X86_64) {
        fail("elf-type");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_phentsize != sizeof(struct EFI_ELF64_PHDR) || eh->e_phnum == 0 ||
        eh->e_phnum > EFI_ELF_PHDR_MAX) {
        fail("elf-phnum");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > got) {
        fail("elf-phdrs");
        return EFI_LOAD_ERROR;
    }

    *img_lo = ~0ull;
    *img_hi = 0;
    for (i = 0; i < eh->e_phnum; i++) {
        const struct EFI_ELF64_PHDR *ph = (const struct EFI_ELF64_PHDR *)
            (uefi_elf_hdr + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        uefi_phdrs[i] = *ph;
        if (ph->p_type != EFI_PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_paddr < *img_lo)
            *img_lo = ph->p_paddr;
        if (ph->p_paddr + ph->p_memsz > *img_hi)
            *img_hi = ph->p_paddr + ph->p_memsz;
    }
    uefi_phnum = eh->e_phnum;
    *entry_va = eh->e_entry;
    if (*img_hi == 0 || *img_lo != EFI_KERNEL_PHYS) {
        fail("elf-layout");
        return EFI_LOAD_ERROR;
    }
    return EFI_SUCCESS;
}

/**
 *  按程序头把内核各段读入物理内存
 *
 *  @param file  已定位到 \kernel.elf 的文件句柄
 *  @param delta kphys - EFI_KERNEL_BASE_PHYS，即映像的物理位移
 *  @returns 成功返回 EFI_SUCCESS；读失败输出错误并返回 EFI_LOAD_ERROR
 */
static EFI_STATUS kernel_copy(struct EFI_FILE_PROTOCOL *file, uint64_t delta) {
    uint32_t i;

    for (i = 0; i < uefi_phnum; i++) {
        const struct EFI_ELF64_PHDR *ph = &uefi_phdrs[i];
        uint8_t *dst;
        uint64_t left;

        if (ph->p_type != EFI_PT_LOAD || ph->p_memsz == 0)
            continue;
        dst = (uint8_t *)(uintptr_t)(ph->p_paddr + delta);
        if (ph->p_filesz > 0) {
            if (file->set_position(file, ph->p_offset) != EFI_SUCCESS) {
                fail("elf-seek");
                return EFI_LOAD_ERROR;
            }
            left = ph->p_filesz;
            while (left > 0) {
                uint64_t chunk = left;
                if (file->read(file, &chunk, dst + (ph->p_filesz - left)) !=
                    EFI_SUCCESS) {
                    fail("elf-read");
                    return EFI_LOAD_ERROR;
                }
                if (chunk == 0) {
                    fail("elf-short");
                    return EFI_LOAD_ERROR;
                }
                left -= chunk;
            }
        }
        if (ph->p_memsz > ph->p_filesz)
            memset(dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }
    return EFI_SUCCESS;
}

/**
 *  选一个 2MB 对齐的随机物理基址并占用内核映像区间
 *
 *  @param img_lo 映像链接期物理起点
 *  @param img_hi 映像链接期物理末端
 *  @returns 成功返回选中的 kphys，全部候选都不可用返回 0
 */
static uint32_t pick_kphys(uint64_t img_lo, uint64_t img_hi) {
    uint64_t pages = (img_hi - img_lo + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    uint32_t slots = (uint32_t)((EFI_KASLR_MAX - EFI_KASLR_MIN) / EFI_PAGE_2M);
    uint32_t start = (uint32_t)((efi_rdtsc() >> 21) % slots);
    uint32_t i;

    for (i = 0; i < slots; i++) {
        uint32_t kphys = (uint32_t)(EFI_KASLR_MIN +
                                    (uint64_t)((start + i) % slots) * EFI_PAGE_2M);
        void *got = (void *)(uintptr_t)((uint64_t)kphys + img_lo);
        EFI_STATUS status = uefi_bs->allocate_pages(EFI_ALLOCATE_ADDRESS,
                                                    EFI_LOADER_DATA, pages, &got);
        if (status == EFI_SUCCESS &&
            (uint64_t)(uintptr_t)got == (uint64_t)kphys + img_lo)
            return kphys;
    }
    return 0;
}

/**
 *  选定显示模式并填充 framebuffer 信息
 *
 *  @returns 成功返回 EFI_SUCCESS；无 GOP 或无可用 RGB/BGR 模式返回非成功
 */
static EFI_STATUS select_gop(void) {
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    uint64_t best_area = 0;
    uint64_t info_size;
    uint32_t max_mode;
    uint32_t best = 0;
    uint32_t m;
    int found = 0;
    EFI_STATUS status;

    status = uefi_bs->locate_protocol(&uefi_guid_gop, NULL, (void **)&gop);
    if (status != EFI_SUCCESS || gop == NULL || gop->mode == NULL) {
        fail("gop");
        return EFI_LOAD_ERROR;
    }

    max_mode = gop->mode->max_mode;
    for (m = 0; m < max_mode; m++) {
        uint64_t area;

        info = NULL;
        if (gop->query_mode(gop, m, &info_size, &info) != EFI_SUCCESS ||
            info == NULL)
            continue;
        if (info->pixel_format == EFI_GOP_PIXEL_RGB ||
            info->pixel_format == EFI_GOP_PIXEL_BGR) {
            if (info->horizontal_resolution == 1024 &&
                info->vertical_resolution == 768) {
                best = m;
                found = 1;
                uefi_bs->free_pool(info);
                break;
            }
            area = (uint64_t)info->horizontal_resolution *
                   info->vertical_resolution;
            if (area > best_area) {
                best_area = area;
                best = m;
                found = 1;
            }
        }
        uefi_bs->free_pool(info);
    }
    if (!found) {
        fail("gop-mode");
        return EFI_LOAD_ERROR;
    }
    if (gop->mode->mode != best && gop->set_mode(gop, best) != EFI_SUCCESS) {
        fail("gop-set");
        return EFI_LOAD_ERROR;
    }

    info = gop->mode->info;
    if (info == NULL ||
        (info->pixel_format != EFI_GOP_PIXEL_RGB &&
         info->pixel_format != EFI_GOP_PIXEL_BGR)) {
        fail("gop-format");
        return EFI_LOAD_ERROR;
    }
    uefi_fb.addr = gop->mode->frame_buffer_base;
    uefi_fb.pitch = info->pixels_per_scan_line * 4u;
    uefi_fb.width = info->horizontal_resolution;
    uefi_fb.height = info->vertical_resolution;
    uefi_fb.bpp = 32;
    uefi_fb.red_size = 8;
    uefi_fb.green_size = 8;
    uefi_fb.blue_size = 8;
    uefi_fb.green_pos = 8;
    if (info->pixel_format == EFI_GOP_PIXEL_RGB) {
        uefi_fb.red_pos = 0;
        uefi_fb.blue_pos = 16;
    } else {
        uefi_fb.red_pos = 16;
        uefi_fb.blue_pos = 0;
    }
    return EFI_SUCCESS;
}

static void build_page_tables(void) {
    uint64_t *pml4 = (uint64_t *)(uintptr_t)EFI_PML4_PHYS;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)EFI_PDPT_PHYS;
    uint64_t *pd_lo = (uint64_t *)(uintptr_t)EFI_PD_LO_PHYS;
    uint64_t *pd_vram = (uint64_t *)(uintptr_t)EFI_PD_VRAM_PHYS;
    uint64_t *pd_mmio = (uint64_t *)(uintptr_t)EFI_PD_MMIO_PHYS;
    uint64_t *pd_hi = (uint64_t *)(uintptr_t)EFI_PD_HI_PHYS;
    uint64_t vram_base = uefi_fb.addr & EFI_VRAM_2M_MASK;
    uint32_t windows = (uint32_t)((uefi_img_hi + EFI_PAGE_2M - 1) / EFI_PAGE_2M);
    uint32_t k;

    memset(pml4, 0, EFI_PAGE_TABLE_PHYS - EFI_PML4_PHYS + 0x1000u);

    pml4[0] = EFI_PDPT_PHYS | EFI_PTE_TABLE_RW;
    pdpt[0] = EFI_PD_LO_PHYS | EFI_PTE_TABLE_RW;
    pdpt[1] = EFI_PD_MMIO_PHYS | EFI_PTE_TABLE_RW;
    pdpt[2] = EFI_PD_VRAM_PHYS | EFI_PTE_TABLE_RW;
    pdpt[3] = EFI_PD_HI_PHYS | EFI_PTE_TABLE_RW;

    for (k = 0; k < EFI_PD_ENTRIES; k++) {
        pd_lo[k] = (uint64_t)k * EFI_PAGE_2M | EFI_PTE_KERN_RW;
        pd_hi[k] = (uint64_t)k * EFI_PAGE_2M | EFI_PTE_KERN_RW;
    }
    for (k = 1; k < windows; k++)
        pd_hi[k] = ((uint64_t)uefi_kphys + (uint64_t)(k - 1) * EFI_PAGE_2M) |
                   EFI_PTE_KERN_RW;

    for (k = 0; k < EFI_VRAM_PAGES; k++)
        pd_vram[k] = (vram_base + (uint64_t)k * EFI_PAGE_2M) | EFI_PTE_KERN_RW;

    pd_mmio[0] = EFI_IOAPIC_PHYS | EFI_PTE_KERN_RW;
    pd_mmio[1] = EFI_LAPIC_PHYS | EFI_PTE_KERN_RW;
}

static void install_landing(void) {
    uint64_t size = (uint64_t)(efi_landing_end - efi_landing_start);

    memcpy((void *)(uintptr_t)EFI_LANDING_PHYS, efi_landing_start, size);
}

/**
 *  抓取固件内存映射
 *
 *  @param key_out 输出，供 ExitBootServices 使用的映射键
 *  @returns 成功返回 EFI_SUCCESS，失败返回固件错误码
 */
static EFI_STATUS grab_memory_map(uint64_t *key_out) {
    uint64_t size = EFI_MMAP_BUFFER_SIZE;
    uint64_t desc_size = 0;
    uint32_t desc_version = 0;
    EFI_STATUS status;

    status = uefi_bs->get_memory_map(&size, uefi_mmap_buf, key_out, &desc_size,
                                     &desc_version);
    if (status != EFI_SUCCESS)
        return status;
    if (desc_size == 0)
        return EFI_LOAD_ERROR;
    uefi_mmap.buf = uefi_mmap_buf;
    uefi_mmap.size = size;
    uefi_mmap.desc_size = desc_size;
    return EFI_SUCCESS;
}

/**
 *  把固件内存类型折算成 multiboot2 类型
 *
 *  @param efi_type 固件 EfiMemoryType
 *  @returns multiboot2 内存类型
 */
static uint32_t mmap_type_of(uint32_t efi_type) {
    switch (efi_type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 7:
        return MB2_MMAP_AVAILABLE;
    case 9:
        return MB2_MMAP_ACPI_RECLAIM;
    case 10:
        return MB2_MMAP_ACPI_NVS;
    default:
        return MB2_MMAP_RESERVED;
    }
}

/**
 *  追加一条已合并的内存映射项
 *
 *  @param type  multiboot2 类型
 *  @param base  起始物理地址
 *  @param len   长度
 */
static void mmap_append(uint32_t type, uint64_t base, uint64_t len) {
    if (uefi_mmap_count > 0) {
        struct MB2_MMAP_ENTRY *last = &uefi_mmap_merged[uefi_mmap_count - 1];
        if (last->type == type && last->addr + last->len == base) {
            last->len += len;
            return;
        }
    }
    if (uefi_mmap_count >= MB2_MAX_MMAP_ENTRIES)
        return;
    uefi_mmap_merged[uefi_mmap_count].addr = base;
    uefi_mmap_merged[uefi_mmap_count].len = len;
    uefi_mmap_merged[uefi_mmap_count].type = type;
    uefi_mmap_merged[uefi_mmap_count].zero = 0;
    uefi_mmap_count++;
}

static void refresh_mmap(void) {
    uint64_t count = uefi_mmap.size / uefi_mmap.desc_size;
    uint64_t low_top = 0;
    uint64_t high_top = 0;
    uint64_t i;
    uint32_t pass;

    uefi_mmap_count = 0;
    uefi_mem_lower_kb = 0;
    uefi_mem_upper_kb = 0;

    for (pass = 0; pass < UEFI_MMAP_PASSES; pass++) {
        for (i = 0; i < count; i++) {
            const struct EFI_MEMORY_DESCRIPTOR *d =
                (const struct EFI_MEMORY_DESCRIPTOR *)(uefi_mmap.buf +
                                                       i * uefi_mmap.desc_size);
            uint32_t type = mmap_type_of(d->type);
            uint64_t base = d->physical_start;
            uint64_t len = d->number_of_pages * EFI_PAGE_SIZE;
            int available = (type == MB2_MMAP_AVAILABLE);

            if (len == 0 || base >= EFI_MAP_LIMIT)
                continue;
            if (base + len > EFI_MAP_LIMIT)
                len = EFI_MAP_LIMIT - base;
            if ((pass == 0) != available)
                continue;
            if (available) {
                uint64_t end = base + len;
                if (base < UEFI_MEM_LOWER_LIMIT) {
                    uint64_t capped = end < UEFI_MEM_LOWER_LIMIT ? end
                                                                 : UEFI_MEM_LOWER_LIMIT;
                    if (capped > low_top)
                        low_top = capped;
                }
                if (end > high_top)
                    high_top = end;
            }
            mmap_append(type, base, len);
        }
    }
    uefi_mem_lower_kb = (uint32_t)(low_top / 1024ull);
    if (high_top > UEFI_MEM_LOWER_LIMIT)
        uefi_mem_upper_kb = (uint32_t)((high_top - UEFI_MEM_LOWER_LIMIT) / 1024ull);
}

/**
 *  在 multiboot2 info 里追加一个标签
 *
 *  @param type 标签类型
 *  @param size 标签总长度（含 8 字节头）
 *  @returns 成功返回标签首地址，空间不足返回 NULL
 */
static void *mb2_add(uint32_t type, uint32_t size) {
    uint32_t step = (size + 7u) & ~7u;
    uint8_t *tag;

    if ((uint64_t)(uefi_mb2_limit - uefi_mb2_cursor) < step)
        return NULL;
    tag = uefi_mb2_cursor;
    memset(tag, 0, step);
    *(uint32_t *)(tag + 0) = type;
    *(uint32_t *)(tag + 4) = size;
    uefi_mb2_cursor += step;
    return tag;
}

static void build_multiboot2(void) {
    struct MB2_TAG_FRAMEBUFFER *fb;
    struct MB2_TAG_BASIC_MEMINFO *mem;
    struct MB2_TAG_ACPI_NEW *acpi_new;
    struct MB2_TAG_ACPI_OLD *acpi_old;
    uint8_t *base = (uint8_t *)(uintptr_t)EFI_MBI_PHYS;
    uint8_t *tag;
    uint8_t *p;
    uint32_t i;

    uefi_mb2_cursor = base + 8;
    uefi_mb2_limit = base + EFI_MBI_MAX_BYTES;

    tag = (uint8_t *)mb2_add(MB2_TYPE_CMDLINE, 8u + sizeof(uefi_cmdline));
    if (tag != NULL)
        memcpy(tag + 8, uefi_cmdline, sizeof(uefi_cmdline));

    tag = (uint8_t *)mb2_add(MB2_TYPE_BOOT_LOADER_NAME,
                             8u + sizeof(uefi_loader_name));
    if (tag != NULL)
        memcpy(tag + 8, uefi_loader_name, sizeof(uefi_loader_name));

    mem = (struct MB2_TAG_BASIC_MEMINFO *)mb2_add(
        MB2_TYPE_BASIC_MEMINFO, sizeof(struct MB2_TAG_BASIC_MEMINFO));
    if (mem != NULL) {
        mem->mem_lower = uefi_mem_lower_kb;
        mem->mem_upper = uefi_mem_upper_kb;
    }

    tag = (uint8_t *)mb2_add(MB2_TYPE_MMAP,
                             16u + uefi_mmap_count *
                                       (uint32_t)sizeof(struct MB2_MMAP_ENTRY));
    if (tag != NULL) {
        *(uint32_t *)(tag + 8) = sizeof(struct MB2_MMAP_ENTRY);
        *(uint32_t *)(tag + 12) = 0;
        p = tag + 16;
        for (i = 0; i < uefi_mmap_count; i++) {
            memcpy(p, &uefi_mmap_merged[i], sizeof(struct MB2_MMAP_ENTRY));
            p += sizeof(struct MB2_MMAP_ENTRY);
        }
    }

    fb = (struct MB2_TAG_FRAMEBUFFER *)mb2_add(
        MB2_TYPE_FRAMEBUFFER, sizeof(struct MB2_TAG_FRAMEBUFFER));
    if (fb != NULL) {
        fb->framebuffer_addr = uefi_fb.addr;
        fb->framebuffer_pitch = uefi_fb.pitch;
        fb->framebuffer_width = uefi_fb.width;
        fb->framebuffer_height = uefi_fb.height;
        fb->framebuffer_bpp = (uint8_t)uefi_fb.bpp;
        fb->framebuffer_type = MB2_FB_TYPE_RGB;
        fb->color_info[0] = uefi_fb.red_pos;
        fb->color_info[1] = uefi_fb.red_size;
        fb->color_info[2] = uefi_fb.green_pos;
        fb->color_info[3] = uefi_fb.green_size;
        fb->color_info[4] = uefi_fb.blue_pos;
        fb->color_info[5] = uefi_fb.blue_size;
    }

    if (uefi_rsdp != NULL && uefi_rsdp_len >= 36) {
        acpi_new = (struct MB2_TAG_ACPI_NEW *)mb2_add(MB2_TYPE_ACPI_NEW,
                                                      sizeof(struct MB2_TAG_ACPI_NEW));
        if (acpi_new != NULL)
            memcpy(acpi_new->rsdp, uefi_rsdp, 36);
    }
    if (uefi_rsdp != NULL) {
        acpi_old = (struct MB2_TAG_ACPI_OLD *)mb2_add(MB2_TYPE_ACPI_OLD,
                                                      sizeof(struct MB2_TAG_ACPI_OLD));
        if (acpi_old != NULL)
            memcpy(acpi_old->rsdp, uefi_rsdp, 20);
    }

    mb2_add(MB2_TYPE_END, 8);

    *(uint32_t *)(base + 0) = (uint32_t)(uefi_mb2_cursor - base);
    *(uint32_t *)(base + 4) = 0;
}

/**
 *  UEFI 应用入口
 *
 *  @param image 本映像的句柄，透传给 ExitBootServices
 *  @param st    系统表
 *  @returns 任何失败路径都返回 EFI_LOAD_ERROR 并已在控制台留下 EFI-ERROR 行；
 *           成功时不会返回，落地代码会带着 multiboot2 约定跳入内核
 */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, struct EFI_SYSTEM_TABLE *st) {
    struct EFI_FILE_PROTOCOL *file = NULL;
    uint64_t entry_va = 0;
    uint64_t img_lo = 0;
    uint64_t img_hi = 0;
    uint64_t map_key = 0;
    uint64_t delta;
    uint32_t tries;
    EFI_STATUS status;

    uefi_image = image;
    uefi_st = st;
    if (st == NULL || st->boot_services == NULL)
        return EFI_LOAD_ERROR;
    uefi_bs = st->boot_services;

    print("corez uefi loader\r\n");

    if (kernel_parse(&file, &entry_va, &img_lo, &img_hi) != EFI_SUCCESS)
        return EFI_LOAD_ERROR;
    uefi_img_hi = img_hi;
    print("elf entry=");
    print_hex(entry_va, 8);
    print(" lo=");
    print_hex(img_lo, 8);
    print(" hi=");
    print_hex(img_hi, 8);
    print("\r\n");

    uefi_kphys = pick_kphys(img_lo, img_hi);
    if (uefi_kphys == 0) {
        fail("kaslr");
        return EFI_LOAD_ERROR;
    }
    print("kphys=");
    print_hex(uefi_kphys, 8);
    print("\r\n");

    delta = (uint64_t)uefi_kphys - EFI_KERNEL_BASE_PHYS;
    status = kernel_copy(file, delta);
    file->close(file);
    if (status != EFI_SUCCESS)
        return EFI_LOAD_ERROR;

    if (select_gop() != EFI_SUCCESS)
        return EFI_LOAD_ERROR;
    print("fb addr=");
    print_hex(uefi_fb.addr, 8);
    print(" ");
    print_dec(uefi_fb.width);
    print("x");
    print_dec(uefi_fb.height);
    print(" pitch=");
    print_dec(uefi_fb.pitch);
    print("\r\n");

    if (reserve(EFI_MBI_PHYS, EFI_MBI_PAGES, "mbi") != EFI_SUCCESS)
        return EFI_LOAD_ERROR;
    if (reserve(EFI_LANDING_PHYS, EFI_LANDING_PAGES, "landing") != EFI_SUCCESS)
        return EFI_LOAD_ERROR;
    if (reserve(EFI_STACK_BASE_PHYS, EFI_STACK_PAGES, "stack") != EFI_SUCCESS)
        return EFI_LOAD_ERROR;
    if (reserve(EFI_PAGE_TABLE_PHYS, EFI_PAGE_TABLE_ALLOC_PAGES, "pagetables") !=
        EFI_SUCCESS)
        return EFI_LOAD_ERROR;

    build_page_tables();
    install_landing();
    find_rsdp();

    status = uefi_bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                     EFI_MMAP_BUFFER_SIZE / EFI_PAGE_SIZE,
                                     (void **)&uefi_mmap_buf);
    if (status != EFI_SUCCESS) {
        fail("mmap-buf");
        return EFI_LOAD_ERROR;
    }

    status = grab_memory_map(&map_key);
    if (status != EFI_SUCCESS) {
        fail("get-mmap");
        return EFI_LOAD_ERROR;
    }
    refresh_mmap();
    print("mmap entries=");
    print_dec(uefi_mmap_count);
    print(" lower=");
    print_dec(uefi_mem_lower_kb);
    print("KB upper=");
    print_dec(uefi_mem_upper_kb);
    print("KB\r\n");
    print("rsdp len=");
    print_dec(uefi_rsdp == NULL ? 0u : uefi_rsdp_len);
    print("\r\n");

    for (tries = 0; tries < UEFI_EXIT_TRIES; tries++) {
        status = uefi_bs->exit_boot_services(uefi_image, map_key);
        if (status == EFI_SUCCESS)
            break;
        status = grab_memory_map(&map_key);
        if (status != EFI_SUCCESS) {
            fail("get-mmap");
            return EFI_LOAD_ERROR;
        }
        refresh_mmap();
    }
    if (status != EFI_SUCCESS) {
        fail("exit-boot");
        return EFI_LOAD_ERROR;
    }

    build_multiboot2();

    uefi_landing.pml4_phys = EFI_PML4_PHYS;
    uefi_landing.entry_va = entry_va;
    uefi_landing.stack_top = EFI_STACK_TOP_PHYS;
    uefi_landing.mbi_phys = (uint32_t)EFI_MBI_PHYS;
    uefi_landing.kphys = uefi_kphys;
    uefi_landing.magic = EFI_BOOT_MAGIC;
    uefi_landing.pad = 0;
    ((efi_landing_fn)(uintptr_t)EFI_LANDING_PHYS)(&uefi_landing);

    return EFI_SUCCESS;
}

/**
 *  内存拷贝
 *
 *  @param dst 目标地址
 *  @param src 源地址
 *  @param len 字节数
 *  @returns dst
 */
void *memcpy(void *dst, const void *src, uint64_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (len > 0) {
        *d++ = *s++;
        len--;
    }
    return dst;
}

/**
 *  内存填充
 *
 *  @param dst   目标地址
 *  @param value 填充字节
 *  @param len   字节数
 *  @returns dst
 */
void *memset(void *dst, int value, uint64_t len) {
    uint8_t *d = (uint8_t *)dst;

    while (len > 0) {
        *d++ = (uint8_t)value;
        len--;
    }
    return dst;
}