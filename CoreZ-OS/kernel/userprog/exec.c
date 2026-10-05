#include "kernel/userprog/exec.h"
#include "arch/cpu.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "kernel/asm/stub.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "kernel/auxv.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/fsapi.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/mm/access.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/syscall/linux_abi.h"
#include "kernel/syscall/win32.h"
#include "kernel/userprog/elf.h"
#include "kernel/userprog/pe.h"
#include "kernel/userprog/process.h"
#include "kernel/fs/file.h"
static const char **exec_env_defaults(void) {
    static const char *root[5] = {"PATH=/bin:/usr/bin:/", "HOME=/root",
                                  "USER=root", "LOGNAME=root", "LANG=C"};
    static const char *user[5] = {"PATH=/bin:/usr/bin", "HOME=/home/user",
                                  "USER=user", "LOGNAME=user", "LANG=C"};
    return (current != NULL && current->euid == 0) ? root : user;
}

#include "lib/rand/rand.h"
#include "lib/str/str.h"
#define EFLAGS_MBS (1 << 1)
#define EFLAGS_IF_1 (1 << 9)
#define EFLAGS_IOPL_0 0
#define MAX_ARG_NR 16
#define MAX_ARG_STR_LEN 256
#define EXEC_STRBUF_HALF (MAX_ARG_NR * MAX_ARG_STR_LEN)
#define EXEC_STRBUF_PAGES 2
#define HEAP_ASLR_PAGES 2048
#define MAX_INTERP_PATH 128

static void apply_rx(uint32_t base, uint32_t pages) {
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t pg = base + i * PAGE_SIZE;
        uint64_t *pte = pte_ptr(pg);
        if (pte != NULL && (*pte & PTE_P)) {
            *pte = (*pte & 0x000ffffffffff000ull) | pte_wx(PTE_P | PTE_U, 0, 1);
            __asm__ volatile("invlpg (%0)" : : "r"(pg) : "memory");
        }
    }
}

static void apply_relocs(uint32_t bias, uint32_t dyn_vaddr) {
    if (bias == 0 || dyn_vaddr == 0)
        return;
    struct LINUX_ELF64_DYN *d = (struct LINUX_ELF64_DYN *)(uintptr_t)(bias + dyn_vaddr);
    uint64_t rela = 0, relasz = 0, relaent = sizeof(struct LINUX_ELF64_RELA);
    uint64_t relr = 0, relrsz = 0;
    for (int i = 0; d[i].d_tag != DT_NULL; i++) {
        if (d[i].d_tag == DT_RELA)
            rela = d[i].d_val;
        else if (d[i].d_tag == DT_RELASZ)
            relasz = d[i].d_val;
        else if (d[i].d_tag == DT_RELAENT)
            relaent = d[i].d_val;
        else if (d[i].d_tag == DT_RELR)
            relr = d[i].d_val;
        else if (d[i].d_tag == DT_RELRSZ)
            relrsz = d[i].d_val;
    }
    if (rela != 0 && relasz != 0) {
        for (uint64_t off = 0; off < relasz; off += relaent) {
            struct LINUX_ELF64_RELA *r =
                (struct LINUX_ELF64_RELA *)(uintptr_t)(bias + rela + off);
            if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE)
                *(uint64_t *)(uintptr_t)(bias + r->r_offset) =
                    bias + r->r_addend;
        }
    }
    if (relr == 0 || relrsz == 0)
        return;
    uint64_t where = 0;
    for (uint64_t off = 0; off < relrsz; off += sizeof(uint64_t)) {
        uint64_t w = *(uint64_t *)(uintptr_t)(bias + relr + off);
        if ((w & 1) == 0) {
            where = w;
            *(uint64_t *)(uintptr_t)(bias + where) += bias;
            where += sizeof(uint64_t);
        } else {
            uint64_t bitmap = w >> 1;
            for (int b = 0; b < 63; b++) {
                if (bitmap & (1ull << b)) {
                    uint64_t a = where + (uint64_t)b * sizeof(uint64_t);
                    *(uint64_t *)(uintptr_t)(bias + a) += bias;
                }
            }
            where += 63 * sizeof(uint64_t);
        }
    }
}

static int brk_page_taken(uint32_t v) {
    uint64_t *pde = pde_ptr(v);
    if (pde == NULL) {
        return 0;
    }
    if (*pde & 0x80) {
        return 1;
    }
    uint64_t *pte = pte_ptr(v);
    return (pte != NULL && (*pte & 1)) ? 1 : 0;
}

static uint32_t pick_brk_base(uint32_t image_end) {
    uint32_t brk_end = (image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (brk_end >= USER_LOW_CEILING) {
        return brk_end;
    }
    uint32_t span = (USER_LOW_CEILING - brk_end) / PAGE_SIZE;
    if (span > HEAP_ASLR_PAGES) {
        span = HEAP_ASLR_PAGES;
    }
    if (span == 0) {
        return brk_end;
    }
    uint32_t gap = (rand_u32() % span) * PAGE_SIZE;
    uint32_t brk_base = brk_end + gap;
    while (brk_base > brk_end && brk_page_taken(brk_base)) {
        brk_base -= PAGE_SIZE;
    }
    return brk_base;
}

static void scan_note_abi(int32_t fd, uint32_t base_off, uint32_t filesz,
                          int *is_linux) {
    sys_lseek(fd, base_off, SEEK_SET);
    uint32_t remaining = filesz;
    while (remaining >= sizeof(struct LINUX_ELF64_NHDR)) {
        struct LINUX_ELF64_NHDR nh;
        if (read_file(fd, &nh, sizeof(nh)) != sizeof(nh))
            break;
        uint32_t sz = sizeof(nh) + ((nh.namesz + 3) & ~3u) +
                      ((nh.descsz + 3) & ~3u);
        if (sz > remaining)
            break;
        if (nh.type == NT_GNU_ABI_TAG && nh.namesz >= 4) {
            char name[4];
            read_file(fd, name, 4);
            if (memcmp(name, "GNU", 4) == 0 && nh.descsz >= 8) {
                uint8_t desc[8];
                read_file(fd, desc, 8);
                if (desc[0] == 0) {
                    *is_linux = 1;
                }
            }
        }
        remaining -= sz;
        sys_lseek(fd, base_off + (filesz - remaining), SEEK_SET);
    }
}

static void fill_entry_regs(struct ARCH_REGS *r, uint32_t entry, int is64,
                            uint32_t rsp, uint32_t argc, uint32_t argv_base,
                            uint32_t envp_base, int is_pe) {
    memset(r, 0, sizeof(struct ARCH_REGS));
    r->rip = entry;
    r->cs = is64 ? SELECTOR_USER64_CODE : SELECTOR_U_CODE;
    r->rflags = EFLAGS_IOPL_0 | EFLAGS_MBS | EFLAGS_IF_1;
    r->user_rsp = rsp;
    r->ss = SELECTOR_U_DATA;
    r->gs_saved = is_pe ? SELECTOR_TLS : 0;
    if (is_pe) {
        r->rcx = argc;
        r->rdx = argv_base;
        r->r8 = envp_base;
        return;
    }
    if (is64) {
        r->rdi = argc;
        r->rsi = argv_base;
    } else {
        r->rbx = argv_base;
        r->rcx = argc;
    }
}

static int32_t segment_load(int32_t fd, uint32_t offset, uint32_t filesz,
                            uint32_t memsz, uint32_t vaddr) {
    uint32_t vaddr_first_page = vaddr & 0xfffff000;
    uint32_t size_in_first_page = PAGE_SIZE - (vaddr & 0x00000fff);
    uint32_t occupy_pages =
        (memsz > size_in_first_page)
            ? DIV_ROUND_UP(memsz - size_in_first_page, PAGE_SIZE) + 1
            : 1;
    uint32_t vaddr_page = vaddr_first_page;
    for (uint32_t i = 0; i < occupy_pages; i++) {
        uint64_t *pde = pde_ptr(vaddr_page);
        uint64_t *pte = pte_ptr(vaddr_page);
        if (pde == NULL || (*pde & 0x80) || pte == NULL || !(*pte & 1)) {
            if (get_a_page(vaddr_page) == 0) {
                return -1;
            }
        } else if (!(*pte & 2)) {
            free_user_page(vaddr_page);
            if (get_a_page(vaddr_page) == 0) {
                return -1;
            }
        }
        vaddr_page += PAGE_SIZE;
    }
    sys_lseek(fd, offset, SEEK_SET);
    if (read_file(fd, (void *)vaddr, filesz) != filesz) {
        kprintf("[exec] segment short read: off=%x want=%x vaddr=%x\n", offset,
                filesz, vaddr);
        return -1;
    }
    return 0;
}

/**
 * 以只读方式打开镜像并校验读权限。
 *
 * @param pathname 镜像路径
 * @returns 文件描述符；不存在返回 -1，无读权限返回 -1 并置 errno=13
 */
static int32_t open_image(const char *pathname) {
    int32_t fd = open_file(pathname, O_RDONLY);
    uint32_t gfd;
    struct FILE *xf;

    if (fd < 0) {
        current->errno = LINUX_ENOENT;
        return -1;
    }
    gfd = fd_local2global((uint32_t)fd);
    xf = file_get(gfd);
    if (xf == NULL || xf->fd_inode == NULL || fs_check_perm(xf->fd_inode, 1u)) {
        close_file(fd);
        current->errno = 13;
        return -1;
    }
    return fd;
}

/**
 * 读入并校验 ELF64 头。
 *
 * @param fd   已打开的镜像
 * @param ehdr 输出，ELF 头
 * @returns 成功返回 0；魔数、位宽、机型、版本或程序头表尺寸非法返回 -1
 */
static int32_t probe_elf64(int32_t fd, struct LINUX_ELF64_EHDR *ehdr) {
    unsigned char ident[16];

    sys_lseek(fd, 0, SEEK_SET);
    if (read_file(fd, ident, sizeof(ident)) != sizeof(ident)) {
        return -1;
    }
    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' ||
        ident[3] != 'F' || ident[4] != 2) {
        return -1;
    }
    sys_lseek(fd, 0, SEEK_SET);
    memset(ehdr, 0, sizeof(*ehdr));
    if (read_file(fd, ehdr, sizeof(*ehdr)) != sizeof(*ehdr)) {
        return -1;
    }
    if ((ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) ||
        ehdr->e_machine != EM_X86_64 || ehdr->e_version != 1 ||
        ehdr->e_phnum > 1024 ||
        ehdr->e_phentsize != sizeof(struct LINUX_ELF64_PHDR)) {
        return -1;
    }
    return 0;
}

/**
 * 读 ELF64 头与 PT_LOAD 覆盖范围，用于挑选互不重叠的加载基址。
 *
 * @param fd    已打开的镜像
 * @param ehdr  输出，ELF 头
 * @param min_v 输出，PT_LOAD 的最低虚拟地址
 * @param span  输出，最低到最高虚拟地址的跨度
 * @returns 成功返回 0；头非法或没有有效 PT_LOAD 返回 -1
 */
static int32_t probe_image64(int32_t fd, struct LINUX_ELF64_EHDR *ehdr,
                             uint32_t *min_v, uint32_t *span) {
    uint32_t lo = 0xffffffffu, hi = 0;
    uint64_t pho;

    if (probe_elf64(fd, ehdr) != 0) {
        return -1;
    }
    pho = ehdr->e_phoff;
    for (uint32_t i = 0; i < ehdr->e_phnum; i++, pho += ehdr->e_phentsize) {
        struct LINUX_ELF64_PHDR ph;
        uint32_t e;

        sys_lseek(fd, pho, SEEK_SET);
        if (read_file(fd, &ph, sizeof(ph)) != sizeof(ph)) {
            return -1;
        }
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        if ((uint32_t)ph.p_vaddr < lo) {
            lo = (uint32_t)ph.p_vaddr;
        }
        e = (uint32_t)(ph.p_vaddr + ph.p_memsz);
        if (e > hi) {
            hi = e;
        }
    }
    if (hi <= lo) {
        return -1;
    }
    *min_v = lo;
    *span = hi - lo;
    return 0;
}

/**
 * 按 PT_LOAD 把 ELF64 镜像映射到 bias + p_vaddr，并把 PF_X 段收成 RX。
 *
 * @param fd      已打开的镜像
 * @param ehdr    已校验的 ELF 头
 * @param bias    加载基址
 * @param bounded 非 0 时跳过落在用户映像区间之外的段（固定地址镜像用）
 * @param end     输出，镜像最高虚拟地址
 * @returns 成功返回 0；映射失败或段越界返回 -1
 *
 * @remarks
 * 只做映射，不做重定位。静态 PIE 的重定位由 apply_relocs 另行完成，
 * 带解释器的动态镜像则整段留给用户态 ld.so
 */
static int32_t map_image64(int32_t fd, const struct LINUX_ELF64_EHDR *ehdr,
                           uint32_t bias, int bounded, uint32_t *end) {
    struct MM_WX_RANGE wx[EXEC_WX_MAX];
    int wxn = 0;
    uint32_t image_end = 0;
    uint64_t pho = ehdr->e_phoff;

    for (uint32_t i = 0; i < ehdr->e_phnum; i++, pho += ehdr->e_phentsize) {
        struct LINUX_ELF64_PHDR ph;
        uint32_t va, map_at, seg_end, first, sz_first;

        sys_lseek(fd, pho, SEEK_SET);
        if (read_file(fd, &ph, sizeof(ph)) != sizeof(ph)) {
            return -1;
        }
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        va = (uint32_t)ph.p_vaddr;
        if (bounded && (va < USER_VADDR_START || va >= USER_STACK3_VADDR)) {
            continue;
        }
        map_at = va + bias;
        if (map_at >= USER_STACK3_VADDR) {
            return -1;
        }
        if (segment_load(fd, (uint32_t)ph.p_offset, (uint32_t)ph.p_filesz,
                         (uint32_t)ph.p_memsz, map_at) == -1) {
            return -1;
        }
        seg_end = map_at + (uint32_t)ph.p_memsz;
        if (seg_end > image_end) {
            image_end = seg_end;
        }
        if ((ph.p_flags & PF_X) && wxn < EXEC_WX_MAX) {
            first = map_at & ~0xfffu;
            sz_first = PAGE_SIZE - (map_at & 0xfffu);
            wx[wxn].base = first;
            wx[wxn].pages = (ph.p_memsz > sz_first)
                                ? DIV_ROUND_UP(ph.p_memsz - sz_first, PAGE_SIZE) + 1
                                : 1;
            wxn++;
        }
    }
    for (int i = 0; i < wxn; i++) {
        apply_rx(wx[i].base, wx[i].pages);
    }
    *end = image_end;
    return 0;
}

/**
 * 读出 PT_INTERP 指定的解释器路径。
 *
 * @param fd      主程序镜像
 * @param ph      已读出的 PT_INTERP 程序头
 * @param buf     输出缓冲区
 * @param buf_len 缓冲区长度
 * @returns 成功返回 0；长度非法、未以 NUL 结尾或不是绝对路径返回 -1
 */
static int32_t read_interp_path(int32_t fd, const struct LINUX_ELF64_PHDR *ph,
                                char *buf, uint32_t buf_len) {
    uint32_t sz = (uint32_t)ph->p_filesz;

    if (sz < 2 || sz > buf_len) {
        return -1;
    }
    sys_lseek(fd, (uint32_t)ph->p_offset, SEEK_SET);
    if (read_file(fd, buf, sz) != sz) {
        return -1;
    }
    buf[sz - 1] = 0;
    return (buf[0] == '/') ? 0 : -1;
}

/**
 * 加载 ELF64 镜像并填好入口、phdr 与堆基址。
 *
 * @param fd  已打开的镜像，由调用者负责关闭
 * @param img 输出，加载结果
 * @returns 成功返回 0；任何校验或映射失败返回 -1
 */
static int32_t load64(int32_t fd, struct EXEC_IMAGE *img) {
    struct LINUX_ELF64_EHDR ehdr;
    struct LINUX_ELF64_EHDR interp_ehdr;
    char interp_path[MAX_INTERP_PATH];
    uint32_t dyn_vaddr = 0;
    uint32_t min_v = 0xffffffffu, max_e = 0;
    uint32_t bias = 0, image_end = 0;
    uint32_t interp_min_v = 0, interp_span = 0, interp_base = 0;
    uint32_t interp_end = 0;
    uint64_t pho;
    int32_t interp_fd = -1;
    int has_interp = 0;
    int is_dyn;
    int rc = -1;

    if (probe_elf64(fd, &ehdr) != 0) {
        return -1;
    }
    is_dyn = (ehdr.e_type == ET_DYN);

    pho = ehdr.e_phoff;
    for (uint32_t i = 0; i < ehdr.e_phnum; i++, pho += ehdr.e_phentsize) {
        struct LINUX_ELF64_PHDR ph;
        uint32_t e;

        sys_lseek(fd, pho, SEEK_SET);
        if (read_file(fd, &ph, sizeof(ph)) != sizeof(ph)) {
            return -1;
        }
        if (ph.p_type == PT_INTERP) {
            if (read_interp_path(fd, &ph, interp_path, sizeof(interp_path)) != 0) {
                return -1;
            }
            has_interp = 1;
        } else if (ph.p_type == PT_DYNAMIC) {
            dyn_vaddr = (uint32_t)ph.p_vaddr;
        } else if (ph.p_type == PT_NOTE) {
            scan_note_abi(fd, (uint32_t)ph.p_offset, (uint32_t)ph.p_filesz,
                          &img->is_linux);
        }
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        if ((uint32_t)ph.p_vaddr < min_v) {
            min_v = (uint32_t)ph.p_vaddr;
        }
        e = (uint32_t)(ph.p_vaddr + ph.p_memsz);
        if (e > max_e) {
            max_e = e;
        }
        if (ehdr.e_phoff >= ph.p_offset &&
            ehdr.e_phoff < ph.p_offset + ph.p_filesz &&
            ((is_dyn && ph.p_vaddr < USER_STACK3_VADDR) ||
             (!is_dyn && ph.p_vaddr >= USER_VADDR_START &&
              ph.p_vaddr < USER_STACK3_VADDR))) {
            img->phdr_vaddr =
                (uint32_t)(ph.p_vaddr + (ehdr.e_phoff - ph.p_offset));
            img->phentsize = ehdr.e_phentsize;
            img->phnum = ehdr.e_phnum;
        }
    }

    if (has_interp) {
        interp_fd = open_image(interp_path);
        if (interp_fd < 0) {
            kprintf("[exec] interp %s: not found or unreadable\n", interp_path);
            return -1;
        }
        if (probe_image64(interp_fd, &interp_ehdr, &interp_min_v,
                          &interp_span) != 0 ||
            interp_ehdr.e_type != ET_DYN) {
            kprintf("[exec] interp %s: not a loadable ET_DYN image\n",
                    interp_path);
            goto out;
        }
    }

    if (is_dyn) {
        uint32_t span = max_e - min_v;
        uint32_t avail = USER_LOW_CEILING - USER_VADDR_START;
        uint32_t reserve = span;
        uint32_t off;

        if (min_v > max_e) {
            goto out;
        }
        if (has_interp) {
            reserve += PAGE_SIZE + DIV_ROUND_UP(interp_span, PAGE_SIZE) * PAGE_SIZE;
        }
        if (reserve >= avail) {
            goto out;
        }
        off = rand_u32() % (avail - reserve);
        off &= ~(PAGE_SIZE - 1);
        bias = USER_VADDR_START + off;
    }

    if (map_image64(fd, &ehdr, bias, !is_dyn, &image_end) != 0) {
        goto out;
    }


    if (has_interp) {
        uint32_t below = (image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t room, gap;

        if (below + interp_span > USER_LOW_CEILING) {
            kprintf("[exec] no room for interp above 0x%x\n", below);
            goto out;
        }
        room = (USER_LOW_CEILING - below - interp_span) / PAGE_SIZE;
        gap = room ? rand_u32() % room : 0;
        interp_base = below + gap * PAGE_SIZE;
        if (map_image64(interp_fd, &interp_ehdr, interp_base - interp_min_v, 0,
                        &interp_end) != 0) {
            goto out;
        }
        if (interp_end > image_end) {
            image_end = interp_end;
        }
    }

    img->bias = bias;
    img->base = bias;
    img->entry = (int32_t)(bias + (uint32_t)ehdr.e_entry);
    img->app_entry = img->entry;
    if (has_interp) {
        uint32_t interp_bias = interp_base - interp_min_v;

        img->has_interp = 1;
        img->base = interp_bias;
        img->entry = (int32_t)(interp_bias + (uint32_t)interp_ehdr.e_entry);
    } else {
        apply_relocs(bias, dyn_vaddr);
    }

    {
        uint32_t gfd2 = fd_local2global((uint32_t)fd);
        struct FILE *xf = file_get(gfd2);
        if (xf != NULL && xf->fd_inode != NULL &&
            (xf->fd_inode->i_mode & 0xF000u) == 0x8000u) {
            uint32_t xm = xf->fd_inode->i_mode;
            struct TASK *xt = current;
            if (xm & 0x800u) {
                xt->euid = xf->fd_inode->i_uid;
                xt->suid = xt->euid;
            }
            if (xm & 0x400u) {
                xt->egid = xf->fd_inode->i_gid;
                xt->sgid = xt->egid;
            }
        }
    }
    if (image_end <= USER_VADDR_START ||
        image_end + PAGE_SIZE >= USER_LOW_CEILING) {
        goto out;
    }
    img->brk_base = pick_brk_base(image_end);
    rc = 0;
out:
    if (interp_fd >= 0) {
        close_file(interp_fd);
    }
    return rc;
}

/**
 * 加载 i386 固定地址镜像。
 *
 * @param fd  已打开的镜像，由调用者负责关闭
 * @param img 输出，加载结果
 * @returns 成功返回 0；校验或映射失败返回 -1
 *
 * @remarks
 * 只接受 ET_EXEC：32 位动态链接不在支持范围内，遇到 PT_INTERP 的镜像
 * 会因为段落在映像区间之外而失败
 */
static int32_t load32(int32_t fd, struct EXEC_IMAGE *img) {
    struct LINUX_ELF32_EHDR elf_header;
    struct LINUX_ELF32_PHDR prog_header;
    uint32_t image_end = 0;
    uint32_t prog_header_offset;

    memset(&elf_header, 0, sizeof(elf_header));
    if (read_file(fd, &elf_header, sizeof(elf_header)) !=
        sizeof(elf_header)) {
        return -1;
    }
    if (memcmp(elf_header.e_ident, "\177ELF\1\1\1", 7) ||
        elf_header.e_type != 2 || elf_header.e_machine != EM_386 ||
        elf_header.e_version != 1 || elf_header.e_phnum > 1024 ||
        elf_header.e_phentsize != sizeof(struct LINUX_ELF32_PHDR)) {
        return -1;
    }

    prog_header_offset = elf_header.e_phoff;
    for (uint32_t prog_idx = 0; prog_idx < elf_header.e_phnum; prog_idx++) {
        memset(&prog_header, 0, sizeof(prog_header));
        sys_lseek(fd, prog_header_offset, SEEK_SET);
        if (read_file(fd, &prog_header, sizeof(prog_header)) !=
            sizeof(prog_header)) {
            return -1;
        }

        if (prog_header.p_type == PT_NOTE) {
            scan_note_abi(fd, prog_header.p_offset, prog_header.p_filesz,
                          &img->is_linux);
            sys_lseek(fd,
                      prog_header_offset +
                          prog_idx * elf_header.e_phentsize +
                          sizeof(struct LINUX_ELF32_PHDR),
                      SEEK_SET);
        }

        if (elf_header.e_phoff >= prog_header.p_offset &&
            elf_header.e_phoff <
                prog_header.p_offset + prog_header.p_filesz &&
            prog_header.p_vaddr >= USER_VADDR_START &&
            prog_header.p_vaddr < USER_STACK3_VADDR) {
            img->phdr_vaddr =
                (uint32_t)(prog_header.p_vaddr +
                           (elf_header.e_phoff - prog_header.p_offset));
            img->phentsize = elf_header.e_phentsize;
            img->phnum = elf_header.e_phnum;
        }
        if (prog_header.p_type == PT_LOAD &&
            prog_header.p_vaddr >= USER_VADDR_START &&
            prog_header.p_vaddr < USER_STACK3_VADDR) {
            if (segment_load(fd, prog_header.p_offset, prog_header.p_filesz,
                             prog_header.p_memsz,
                             prog_header.p_vaddr) == -1) {
                return -1;
            }
            uint32_t seg_end = prog_header.p_vaddr + prog_header.p_memsz;
            if (seg_end > image_end) {
                image_end = seg_end;
            }
            if (prog_header.p_flags & PF_X) {
                uint32_t first = prog_header.p_vaddr & ~0xfffu;
                uint32_t sz_first = PAGE_SIZE - (prog_header.p_vaddr & 0xfffu);
                uint32_t pages =
                    (prog_header.p_memsz > sz_first)
                        ? DIV_ROUND_UP(prog_header.p_memsz - sz_first,
                                       PAGE_SIZE) +
                              1
                        : 1;
                apply_rx(first, pages);
            }
        }
        prog_header_offset += elf_header.e_phentsize;
    }
    img->entry = (int32_t)elf_header.e_entry;
    img->app_entry = img->entry;
    if (image_end <= USER_VADDR_START ||
        image_end + PAGE_SIZE >= USER_LOW_CEILING) {
        return -1;
    }
    img->brk_base = pick_brk_base(image_end);
    return 0;
}

/**
 * 打开并加载镜像。
 *
 * @param pathname 可执行文件路径
 * @param img      输出，加载结果
 * @returns 成功返回进程入口地址；失败返回 -1
 *
 * @remarks
 * 解释器路径等仅在内核态使用，不写入用户可见状态
 */
static int32_t load(const char *pathname, struct EXEC_IMAGE *img) {
    unsigned char ident[16];
    int32_t fd = open_image(pathname);
    int32_t rc;

    if (fd < 0) {
        return -1;
    }
    memset(img, 0, sizeof(*img));
    if (read_file(fd, ident, sizeof(ident)) != sizeof(ident)) {
        current->errno = LINUX_ENOEXEC;
        goto fail;
    }
    if (ident[0] == 'M' && ident[1] == 'Z') {
        if (pe_load(fd, img) != 0) {
            current->errno = LINUX_ENOEXEC;
            goto fail;
        }
        close_file(fd);
        return img->entry;
    }
    if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' ||
        ident[3] != 'F' || (ident[4] != 1 && ident[4] != 2)) {
        current->errno = LINUX_ENOEXEC;
        goto fail;
    }
    img->is64 = (ident[4] == 2);
    rc = img->is64 ? load64(fd, img) : load32(fd, img);
    if (rc != 0) {
        current->errno = LINUX_ENOEXEC;
        goto fail;
    }
    close_file(fd);
    return img->entry;
fail:
    close_file(fd);
    return -1;
}

/**
 * 把 argv/envp 指针数组与各字符串原子拷入内核缓冲
 *
 * @param offs 输出第 i 个字符串在 buf 内的偏移
 * @param bufcap buf 容量
 * @returns 字符串个数；-1 表示任一指针不可达或超出缓冲
 */
static int copy_strs(const char *const *strs, uint32_t *lens, char *buf,
                     uint32_t bufcap, int kcaller, uint32_t *offs) {
    int n = 0;
    uint32_t used = 0;
    if (strs == NULL) {
        return 0;
    }
    while (n < MAX_ARG_NR) {
        const char *s;
        if (!kcaller) {
            char *sp;

            if (copy_from_user(&sp, &strs[n], sizeof(char *)) != 0) {
                return -1;
            }
            if (sp == NULL) {
                break;
            }
            uint32_t room = (bufcap - used < MAX_ARG_STR_LEN)
                                ? (bufcap - used)
                                : MAX_ARG_STR_LEN;
            int len = copy_str_from_user_len(buf + used, sp, room);
            if (len < 0) {
                return -1;
            }
            uint32_t need = (uint32_t)len + 1;
            lens[n] = need;
            offs[n] = used;
            used += need;
        } else {
            s = strs[n];
            if (s == NULL) {
                break;
            }
            uint32_t len = (uint32_t)strlen(s);
            if (len >= MAX_ARG_STR_LEN) {
                return -1;
            }
            uint32_t need = len + 1;
            if (used + need > bufcap) {
                return -1;
            }
            memcpy(buf + used, s, need);
            lens[n] = need;
            offs[n] = used;
            used += need;
        }
        n++;
    }
    return n;
}

int32_t sys_execve(const char *path, const char *argv[], const char *envp[],
                   struct ARCH_REGS *regs) {
    uint32_t argc;
    int32_t entry_point;
    struct TASK *cur;
    uint32_t old_pml4_phys;
    uint32_t ustack_ptr;
    uint32_t argv_user_addrs[MAX_ARG_NR];
    uint32_t slens[MAX_ARG_NR];
    uint32_t envlens[MAX_ARG_NR];
    int32_t envc = 0;
    uint32_t argv_user_base;
    uint32_t envp_user_base;
    int32_t i;
    uint32_t slen;
    struct ARCH_REGS *ps;
    struct EXEC_IMAGE img;
    int is64 = 0;
    int is_linux = 0;
    uint32_t aux_random_addr = 0;
    uint32_t aux_app_entry = 0;
    uint32_t aux_base = 0;

    char *strbuf = NULL;
    uint32_t argv_offs[MAX_ARG_NR];
    uint32_t envp_offs[MAX_ARG_NR];
    cur = current;
    cur->errno = 0;
    old_pml4_phys = cur->pml4_phys;
    if (cur->pml4_phys != 0) {
        space_detach_others(cur);
    }
    if (cur->pml4_phys == 0) {
        cur->pml4_phys = (uint32_t)create_page_dir();
        if (cur->pml4_phys == 0) {
            cur->errno = LINUX_ENOMEM;
            return -1;
        }
        space_ref(cur->pml4_phys);
        process_activate(cur);
    }
    if (cur->userprog_v_addr.vaddr_bitmap.bits != NULL) {
        uint32_t bitmap_bytes =
            cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len;
        uint32_t bitmap_pg_cnt = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint32_t i = 0; i < bitmap_pg_cnt; i++) {
            free_kernel_page((uint32_t)cur->userprog_v_addr.vaddr_bitmap.bits +
                             i * PAGE_SIZE);
        }
        cur->userprog_v_addr.vaddr_bitmap.bits = NULL;
    }
    create_user_vaddr_bitmap(cur);
    {
        int kcaller = (regs != NULL) ? ((regs->cs & 3) == 0) : 1;
        strbuf = (char *)get_kernel_pages(EXEC_STRBUF_PAGES);
        if (strbuf == NULL) {
            cur->errno = LINUX_ENOMEM;
            kprintf("[exec] strbuf alloc failed\n");
            goto exec_fail;
        }
        argc = copy_strs(argv, slens, strbuf, EXEC_STRBUF_HALF, kcaller,
                         argv_offs);
        if (argc < 0) {
            cur->errno = LINUX_E2BIG;
            goto exec_fail;
        }
        const char **env_def = exec_env_defaults();
        if (envp == NULL) {
                envc = 5;
                for (int ed_i = 0; ed_i < 5; ed_i++)
                    envlens[ed_i] =
                        (uint32_t)strlen(env_def[ed_i]) + 1;
        } else {
            envc = copy_strs(envp, envlens,
                             strbuf + EXEC_STRBUF_HALF, EXEC_STRBUF_HALF,
                             kcaller, envp_offs);
            if (envc < 0) {
                cur->errno = LINUX_E2BIG;
                goto exec_fail;
            }
        }
    }
    entry_point = load(path, &img);
    if (entry_point == -1) {
        goto exec_fail;
    }
    is64 = img.is64;
    is_linux = img.is_linux;
    aux_app_entry = (uint32_t)img.app_entry;
    aux_base = img.base;
    memcpy(cur->name, path, 15);
    cur->name[15] = 0;
    {
        char abs[MAX_PATH_LEN];
        const char *src = path;
        if (fs_abs_path(path, abs, sizeof(abs)) == 0) {
            src = abs;
        }
        uint32_t el;
        for (el = 0; el < sizeof(cur->exe_path) - 1 && src[el] != 0; el++) {
            cur->exe_path[el] = src[el];
        }
        cur->exe_path[el] = 0;
    }
    cur->exe_bias = img.base;
    cur->user_brk = 0;
    cur->brk_base = img.brk_base;
    signal_reset_user(cur);
    for (uint32_t sp = USER_STACK_BOTTOM; sp < USER_STACK_TOP;
         sp += PAGE_SIZE) {
        uint64_t *pde = pde_ptr(sp);
        uint64_t *pte = pte_ptr(sp);
        if (pde == NULL || pte == NULL || !(*pte & 1)) {

            if (get_a_page(sp) == 0) {
                cur->errno = LINUX_ENOMEM;
                kprintf("[exec] get_a_page for user stack failed\n");
                goto exec_fail;
            }
        }
    }
    cur->tls_base = 0;
    cur->tls_selector = 0;
    cur->tls_msr = 0;
    cur->errno = 0;
    cur->compat = is_linux;
    cur->stack_bottom = USER_STACK_BOTTOM;
    {
        uint32_t below = rand_u32() % (USER_STACK_PAGES - 3);
        uint32_t sub = (rand_u32() % (PAGE_SIZE / 8)) * 8;
        ustack_ptr = USER_STACK_TOP - below * PAGE_SIZE - sub;
    }
    {
        uint64_t r0 = rand_u64();
        uint64_t r1 = rand_u64();
        ustack_ptr -= 16;
        uint32_t *rnd = (uint32_t *)ustack_ptr;
        rnd[0] = (uint32_t)r0;
        rnd[1] = (uint32_t)(r0 >> 32);
        rnd[2] = (uint32_t)r1;
        rnd[3] = (uint32_t)(r1 >> 32);
        aux_random_addr = ustack_ptr;
    }
    for (i = 0; i < MAX_ARG_NR; ++i) {
        argv_user_addrs[i] = 0;
    }
    if (argc > 0) {
        for (i = (int32_t)argc - 1; i >= 0; --i) {
            slen = slens[i];
            ustack_ptr -= slen;
            ustack_ptr &= ~(is64 ? 0x7u : 0x3u);
            if (ustack_ptr < cur->stack_bottom) {
                cur->errno = LINUX_E2BIG;
                kprintf("[exec] argv too large for user stack\n");
                goto exec_fail;
            }
            memcpy((void *)ustack_ptr, strbuf + argv_offs[i], slen);
            argv_user_addrs[i] = ustack_ptr;
        }
    }
    {
        const char *exefn = path;
        uint32_t envp_addrs[MAX_ARG_NR];
        uint32_t exefn_addr = 0;
        uint32_t aux_dst = 0;
        uint32_t aw = is64 ? 8u : 4u;
        uint32_t amask = is64 ? 0x7u : 0x3u;
        uint64_t aux[48];
        int naw = 0;
        int e;
#define PVAL(p, v)                                                             \
    do {                                                                       \
        if (is64)                                                              \
            *(uint64_t *)(p) = (uint64_t)(v);                                  \
        else                                                                   \
            *(uint32_t *)(p) = (uint32_t)(v);                                  \
    } while (0)
#define PSTACK(sp, v)                                                          \
    do {                                                                       \
        (sp) -= aw;                                                            \
        PVAL(sp, v);                                                           \
    } while (0)
#define A(t, v)                                                                \
    do {                                                                       \
        aux[naw++] = (uint64_t)(t);                                            \
        aux[naw++] = (uint64_t)(v);                                            \
    } while (0)
        A(AT_EXECFN, 0);
        A(AT_PAGESZ, PAGE_SIZE);
        A(AT_CLKTCK, 100);
        A(AT_ENTRY, aux_app_entry);
        A(AT_PHDR, img.phdr_vaddr + (is64 ? img.bias : 0));
        A(AT_PHENT, img.phentsize);
        A(AT_PHNUM, img.phnum);
        A(AT_UID, cur->uid);
        A(AT_EUID, cur->euid);
        A(AT_GID, cur->gid);
        A(AT_EGID, cur->egid);
        A(AT_SECURE, 0);
        if (is64)
            A(AT_BASE, aux_base);
        A(AT_FLAGS, 0);
        A(AT_HWCAP, 0);
        A(AT_RANDOM, aux_random_addr);
        A(AT_NULL, 0);
#undef A

        slen = strlen(exefn) + 1;
        ustack_ptr -= slen;
        ustack_ptr &= ~amask;
        if (ustack_ptr < cur->stack_bottom) {
            cur->errno = LINUX_E2BIG;
            goto exec_fail;
        }
        memcpy((void *)ustack_ptr, exefn, slen);
        exefn_addr = ustack_ptr;

        for (e = envc - 1; e >= 0; --e) {
            slen = envlens[e];
            ustack_ptr -= slen;
            ustack_ptr &= ~amask;
            if (ustack_ptr < cur->stack_bottom) {
                cur->errno = LINUX_E2BIG;
                goto exec_fail;
            }
            memcpy((void *)ustack_ptr,
                   envp ? strbuf + EXEC_STRBUF_HALF + envp_offs[e]
                        : exec_env_defaults()[e],
                   slen);
            envp_addrs[e] = ustack_ptr;
        }

        ustack_ptr &= ~amask;
        if (is64 && ((ustack_ptr -
                      (uint32_t)(naw + envc + (int32_t)argc + 3) * 8u) & 0xFu))
            ustack_ptr -= 8;
        ustack_ptr -= (uint32_t)naw * aw;
        aux_dst = ustack_ptr;
        if (ustack_ptr < cur->stack_bottom) {
            cur->errno = LINUX_E2BIG;
            goto exec_fail;
        }
        aux[1] = exefn_addr;
        for (int k = 0; k < naw; k++) {
            PVAL(aux_dst + (uint32_t)k * aw, aux[k]);
        }

        PSTACK(ustack_ptr, 0);
        for (e = envc - 1; e >= 0; --e)
            PSTACK(ustack_ptr, envp_addrs[e]);
        PSTACK(ustack_ptr, 0);
        for (i = (int32_t)argc - 1; i >= 0; --i)
            PSTACK(ustack_ptr, argv_user_addrs[i]);
        PSTACK(ustack_ptr, argc);
        argv_user_base = ustack_ptr + aw;
        envp_user_base = argv_user_base + (argc + 1u) * aw;
#undef PVAL
#undef PSTACK
    }
    if (img.is_pe && (ustack_ptr & 0xfu) == 0)
        ustack_ptr -= 8;
    goto exec_done;
exec_fail:
    if (strbuf != NULL) {
        free_kernel_page((uint32_t)strbuf);
        free_kernel_page((uint32_t)strbuf + PAGE_SIZE);
        strbuf = NULL;
    }
    if (cur->pml4_phys != old_pml4_phys) {
        uint32_t abandoned = cur->pml4_phys;
        cur->pml4_phys = old_pml4_phys;
        process_activate(cur);
        free_user_space(cur, abandoned);
    } else if (old_pml4_phys == 0 &&
               cur->userprog_v_addr.vaddr_bitmap.bits != NULL) {
        free_user_space(cur, 0);
    }
    return -1;
exec_done:
    if (strbuf != NULL) {
        free_kernel_page((uint32_t)strbuf);
        free_kernel_page((uint32_t)strbuf + PAGE_SIZE);
        strbuf = NULL;
    }
    for (int32_t fd = 3; fd < MAX_FILES_OPEN_PER_PROC; ++fd) {
        if (cur->fd_table[fd] != (uint32_t)-1 && (cur->fd_cloexec >> fd) & 1) {
            cur->fd_cloexec &= ~(1ull << fd);
            close_file(fd);
        }
    }
    if (img.is_pe && img.pe_rt != 0) {
        uint32_t rt = img.pe_rt;
        *(uint32_t *)(uintptr_t)(rt + WIN_RT_ARGC) = argc;
        *(uint32_t *)(uintptr_t)(rt + WIN_RT_ARGV) = argv_user_base;
        *(uint32_t *)(uintptr_t)(rt + WIN_RT_ENVP) = envp_user_base;
        cur->tls_base = rt;
        cur->tls_selector = SELECTOR_TLS;
        cur->tls_msr = 0;
        tls_desc_set_base(rt);
    }
    if (regs != NULL) {
        fill_entry_regs(regs, (uint32_t)entry_point, is64, ustack_ptr, argc,
                        argv_user_base, envp_user_base, img.is_pe);
        return 0;
    }

    ps =
        (struct ARCH_REGS *)(cur->kernel_stack_top - THREAD_STACK_SIZE + 0x100);
    fill_entry_regs(ps, (uint32_t)entry_point, is64, ustack_ptr, argc,
                    argv_user_base, envp_user_base, img.is_pe);

    __asm__ volatile("mov %0, %%ds; mov %0, %%es; mov %0, %%fs;" ::"r"(
                         (uint16_t)SELECTOR_U_DATA)
                     : "memory");
    __asm__ volatile("mov %0, %%rsp; jmp intr_exit" : : "r"(ps) : "memory");
    return 0;
}

int32_t sys_execv(const char *path, const char *argv[],
                  struct ARCH_REGS *regs) {
    return sys_execve(path, argv, NULL, regs);
}
