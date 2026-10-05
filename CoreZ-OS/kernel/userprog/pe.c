#include "kernel/userprog/pe.h"
#include "drivers/char/console/io.h"
#include "kernel/fs/fs.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/syscall/win32.h"
#include "kernel/userprog/process.h"
#include "lib/str/str.h"

struct PE_DOS {
    uint8_t pad[0x3c];
    uint32_t lfanew;
};

struct PE_COFF {
    uint16_t machine;
    uint16_t nsec;
    uint32_t timestamp;
    uint32_t symtab;
    uint32_t nsym;
    uint16_t opt_size;
    uint16_t chars;
};

struct PE_DIR {
    uint32_t rva;
    uint32_t size;
};

struct PE_OPTHDR {
    uint16_t magic;
    uint8_t major;
    uint8_t minor;
    uint32_t size_code;
    uint32_t size_init;
    uint32_t size_uninit;
    uint32_t entry;
    uint32_t base_code;
    uint64_t image_base;
    uint32_t sec_align;
    uint32_t file_align;
    uint16_t os_major;
    uint16_t os_minor;
    uint16_t img_major;
    uint16_t img_minor;
    uint16_t subsys_major;
    uint16_t subsys_minor;
    uint32_t win32ver;
    uint32_t size_image;
    uint32_t size_headers;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_chars;
    uint64_t stack_reserve;
    uint64_t stack_commit;
    uint64_t heap_reserve;
    uint64_t heap_commit;
    uint32_t loader_flags;
    uint32_t nrva;
    struct PE_DIR dir[16];
};

struct PE_SECRAW {
    char name[8];
    uint32_t vsize;
    uint32_t va;
    uint32_t raw_size;
    uint32_t raw_off;
    uint32_t reloc_off;
    uint32_t lineno_off;
    uint16_t nreloc;
    uint16_t nlineno;
    uint32_t chars;
};

struct PE_IMPDESC {
    uint32_t oft;
    uint32_t timestamp;
    uint32_t forwarder;
    uint32_t name;
    uint32_t ft;
};

struct PE_RELOCBLK {
    uint32_t va;
    uint32_t size;
};

static int32_t pe_read(int32_t fd, uint32_t off, void *buf, uint32_t len) {
    if (sys_lseek(fd, (int32_t)off, SEEK_SET) < 0)
        return -1;
    return (read_file(fd, buf, len) == len) ? 0 : -1;
}

static void pe_apply_rx(uint32_t base, uint32_t pages) {
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t pg = base + i * PAGE_SIZE;
        uint64_t *pte = pte_ptr(pg);
        if (pte != NULL && (*pte & PTE_P)) {
            *pte = (*pte & 0x000ffffffffff000ull) | pte_wx(PTE_P | PTE_U, 0, 1);
            __asm__ volatile("invlpg (%0)" : : "r"(pg) : "memory");
        }
    }
}

static int pe_map_page(uint32_t pg) {
    uint64_t *pde = pde_ptr(pg);
    uint64_t *pte = pte_ptr(pg);
    if (pde == NULL || (*pde & PTE_PS) || pte == NULL || !(*pte & PTE_P)) {
        if (get_a_page(pg) == 0)
            return -1;
    }
    return 0;
}

static int pe_map_sec(int32_t fd, uint32_t base, const struct PE_SECTION *s) {
    uint32_t va = base + s->va;
    uint32_t vsize = (s->vsize != 0) ? s->vsize : s->raw_size;
    uint32_t first;
    uint32_t pages;
    if (vsize == 0)
        return 0;
    first = va & ~(PAGE_SIZE - 1);
    pages = DIV_ROUND_UP((va - first) + vsize, PAGE_SIZE);
    for (uint32_t i = 0; i < pages; i++) {
        if (pe_map_page(first + i * PAGE_SIZE) != 0)
            return -1;
    }
    if (s->raw_size != 0) {
        uint32_t copy = (s->raw_size < vsize) ? s->raw_size : vsize;
        if (pe_read(fd, s->raw_off, (void *)(uintptr_t)va, copy) != 0)
            return -1;
    }
    return 0;
}

static void pe_relocs(const struct PE_IMAGE *img) {
    int64_t delta = (int64_t)img->base - (int64_t)img->image_base;
    uint32_t off = 0;
    if (delta == 0 || img->reloc_rva == 0 || img->reloc_size == 0)
        return;
    while (off + sizeof(struct PE_RELOCBLK) <= img->reloc_size) {
        const struct PE_RELOCBLK *blk =
            (const struct PE_RELOCBLK *)(uintptr_t)(img->base + img->reloc_rva +
                                                    off);
        uint32_t total = blk->size;
        const uint16_t *ent;
        uint32_t cnt;
        if (total < sizeof(struct PE_RELOCBLK))
            break;
        cnt = (total - (uint32_t)sizeof(struct PE_RELOCBLK)) / 2u;
        ent = (const uint16_t *)(const void *)(blk + 1);
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t type = ent[i] >> 12;
            uint32_t roff = ent[i] & 0xfffu;
            if (type == PE_RELOC_DIR64) {
                uint64_t *slot =
                    (uint64_t *)(uintptr_t)(img->base + blk->va + roff);
                *slot = (uint64_t)((int64_t)*slot + delta);
            }
        }
        off += total;
    }
}

static void pe_imports(const struct PE_IMAGE *img) {
    const struct PE_IMPDESC *d;
    if (img->import_rva == 0 || img->import_size == 0)
        return;
    d = (const struct PE_IMPDESC *)(uintptr_t)(img->base + img->import_rva);
    for (uint32_t n = 0; n < PE_IMPORT_MAX; n++) {
        uint32_t ilt;
        const char *mod;
        if (d->name == 0 || d->ft == 0)
            break;
        mod = (const char *)(uintptr_t)(img->base + d->name);
        ilt = (d->oft != 0) ? d->oft : d->ft;
        for (uint32_t k = 0; k < 0x4000u; k++) {
            const uint64_t *slot =
                (const uint64_t *)(uintptr_t)(img->base + ilt + k * 8u);
            uint64_t e = *slot;
            const char *name;
            if (e == 0)
                break;
            if (e & 0x8000000000000000ull)
                continue;
            name = (const char *)(uintptr_t)(img->base + (uint32_t)e + 2u);
            *(uint64_t *)(uintptr_t)(img->base + d->ft + k * 8u) =
                (uint64_t)win32_resolve(mod, name);
        }
        d++;
    }
}

int32_t pe_load(int32_t fd, struct EXEC_IMAGE *out) {
    struct PE_IMAGE pe;
    struct PE_IMAGE *img = &pe;
    struct PE_DOS dos;
    struct PE_COFF coff;
    struct PE_OPTHDR opt;
    struct PE_SECRAW sr;
    struct PE_SECTION hdr;
    uint32_t lfanew;
    uint32_t img_base;
    uint32_t size_image;
    uint32_t sec_off;
    uint32_t thunk;
    uint32_t rt;
    uint32_t osz;

    if (pe_read(fd, 0, &dos, sizeof(dos)) != 0)
        return -1;
    if (dos.lfanew < 0x40u || dos.lfanew >= 0x1000u)
        return -1;
    lfanew = dos.lfanew;
    if (pe_read(fd, lfanew + 4u, &coff, sizeof(coff)) != 0)
        return -1;
    if (coff.machine != PE_MACHINE_AMD64)
        return -1;
    if (coff.nsec == 0 || coff.nsec > PE_SECTION_MAX || coff.opt_size == 0)
        return -1;
    osz = coff.opt_size;
    if (osz > sizeof(opt))
        osz = sizeof(opt);
    memset(&opt, 0, sizeof(opt));
    if (pe_read(fd, lfanew + 24u, &opt, osz) != 0)
        return -1;
    if (opt.magic != PE_MAGIC_PE32P)
        return -1;
    size_image = opt.size_image;
    if (size_image == 0)
        return -1;
    if (opt.image_base >= USER_VADDR_START &&
        opt.image_base + size_image <= USER_LOW_CEILING &&
        (opt.image_base & (PAGE_SIZE - 1)) == 0) {
        img_base = (uint32_t)opt.image_base;
    } else {
        img_base = PE_FALLBACK_BASE;
    }
    if (img_base + size_image >= USER_LOW_CEILING)
        return -1;
    memset(img, 0, sizeof(*img));
    img->image_base = opt.image_base;
    img->base = img_base;
    img->entry = (uint32_t)(img_base + opt.entry);
    img->app_entry = img->entry;
    img->size_image = size_image;
    img->size_headers = opt.size_headers;
    img->nsec = coff.nsec;
    if (opt.nrva > 1) {
        img->import_rva = opt.dir[1].rva;
        img->import_size = opt.dir[1].size;
    }
    if (opt.nrva > 5) {
        img->reloc_rva = opt.dir[5].rva;
        img->reloc_size = opt.dir[5].size;
    }
    sec_off = lfanew + 24u + coff.opt_size;
    for (uint32_t i = 0; i < coff.nsec; i++) {
        if (pe_read(fd, sec_off + i * (uint32_t)sizeof(sr), &sr,
                    (uint32_t)sizeof(sr)) != 0)
            return -1;
        img->sec[i].va = sr.va;
        img->sec[i].vsize = sr.vsize;
        img->sec[i].raw_off = sr.raw_off;
        img->sec[i].raw_size = sr.raw_size;
        img->sec[i].flags = sr.chars;
    }
    img->image_end = img_base + size_image;
    thunk = (img->image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (get_a_page(thunk) == 0)
        return -1;
    win32_thunk_init(thunk);
    pe_apply_rx(thunk, 1);
    img->thunk_base = thunk;
    rt = thunk + PAGE_SIZE;
    if (get_a_page(rt) == 0)
        return -1;
    memset((void *)(uintptr_t)rt, 0, PAGE_SIZE);
    *(uint64_t *)(uintptr_t)(rt + WIN_TEB_STACK_BASE) = USER_STACK_TOP;
    *(uint64_t *)(uintptr_t)(rt + WIN_TEB_STACK_LIMIT) = USER_STACK_BOTTOM;
    *(uint64_t *)(uintptr_t)(rt + WIN_TEB_SELF) = rt;
    img->rt = rt;
    if (opt.size_headers != 0) {
        hdr.va = 0;
        hdr.vsize = opt.size_headers;
        hdr.raw_off = 0;
        hdr.raw_size = opt.size_headers;
        hdr.flags = 0;
        if (pe_map_sec(fd, img_base, &hdr) != 0)
            return -1;
    }
    for (uint32_t i = 0; i < coff.nsec; i++) {
        if (pe_map_sec(fd, img_base, &img->sec[i]) != 0)
            return -1;
    }
    pe_relocs(img);
    pe_imports(img);
    for (uint32_t i = 0; i < coff.nsec; i++) {
        uint32_t va;
        uint32_t first;
        uint32_t size;
        uint32_t pages;
        if (!(img->sec[i].flags & PE_SCN_MEM_EXECUTE))
            continue;
        size = (img->sec[i].vsize != 0) ? img->sec[i].vsize
                                        : img->sec[i].raw_size;
        if (size == 0)
            continue;
        va = img_base + img->sec[i].va;
        first = va & ~(PAGE_SIZE - 1);
        pages = DIV_ROUND_UP((va - first) + size, PAGE_SIZE);
        pe_apply_rx(first, pages);
    }
    img->brk_base = rt + PAGE_SIZE;
    out->entry = (int32_t)img->entry;
    out->app_entry = img->app_entry;
    out->base = img->base;
    out->brk_base = img->brk_base;
    out->is64 = 1;
    out->is_pe = 1;
    out->pe_rt = rt;
    current->win_base = img_base;
    current->win_rt = rt;
    return 0;
}