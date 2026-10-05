#include "kernel/mm/access.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"
#define USER_VADDR_BEGIN 0x8048000u
static int user_range_walk(uint32_t addr, uint32_t len, int write);
int access_ok(const void *addr, size_t n, int write) {
    if (n == 0) {
        return 1;
    }
    uint32_t a = (uint32_t)(uintptr_t)addr;
    if (a < USER_VADDR_BEGIN || a >= USER_SPACE_END ||
        n > (size_t)(USER_SPACE_END - a)) {
        return 0;
    }
    return user_range_walk(a, (uint32_t)n, write);
}

static int user_page_readable(uint32_t a) {
    uint64_t *pde = pde_ptr(a);
    if (pde == NULL || !(*pde & PTE_P)) {
        return 0;
    }
    if (*pde & PTE_PS) {
        return (*pde & PTE_U) != 0;
    }
    uint64_t *pte = pte_ptr(a);
    return (*pte & PTE_P) && (*pte & PTE_U);
}

static int user_page_writable(uint32_t a) {
    uint64_t *pde = pde_ptr(a);
    if (pde == NULL || !(*pde & PTE_P)) {
        return 0;
    }
    if (*pde & PTE_PS) {
        return (*pde & (PTE_U | PTE_W)) == (PTE_U | PTE_W);
    }
    uint64_t *pte = pte_ptr(a);
    if (!(*pte & PTE_P) || !(*pte & PTE_U)) {
        return 0;
    }

    if (!(*pte & PTE_W) &&
        (!(*pte & COW_FLAG) || !page_cow_resolve(a, *pte))) {
        return 0;
    }
    return 1;
}

static int user_str_span(const char *src, uint32_t max, char *dst) {
    if (src == NULL || max == 0) {
        return -1;
    }
    uint32_t a = (uint32_t)(uintptr_t)src;
    if (a < USER_VADDR_BEGIN || a >= USER_SPACE_END) {
        return -1;
    }
    uint32_t off = 0;
    while (off < max) {
        uint32_t va = a + off;
        if (!user_page_readable(va)) {
            return -1;
        }
        uint32_t room = PAGE_SIZE - (va & 0xFFFu);
        uint32_t n = (room < max - off) ? room : (max - off);
        const char *s = (const char *)(uintptr_t)va;
        for (uint32_t i = 0; i < n; i++) {
            char c = s[i];
            if (dst) {
                dst[off + i] = c;
            }
            if (c == 0) {
                return (int)(off + i);
            }
        }
        off += n;
    }
    return -1;
}

int copy_str_from_user(char *dst, const char *src, uint32_t max) {
    return user_str_span(src, max, dst) < 0 ? -1 : 0;
}

int copy_str_from_user_len(char *dst, const char *src, uint32_t max) {
    return user_str_span(src, max, dst);
}

/**
 * 把用户空间数据原子拷入内核缓冲，逐页校验可读后立即拷贝该页，校验与访问
 *
 * @param dst 内核缓冲
 * @param src 用户地址
 * @param len 字节数
 * @returns 0 成功；-1 任一页不可读或地址越界
 */
int copy_from_user(void *dst, const void *src, uint32_t len) {
    if (len == 0) {
        return 0;
    }
    uint32_t a = (uint32_t)(uintptr_t)src;
    if (a < USER_VADDR_BEGIN || a >= USER_SPACE_END ||
        len > USER_SPACE_END - a) {
        return -1;
    }
    uint8_t *d = (uint8_t *)dst;
    uint32_t off = 0;
    while (off < len) {
        uint32_t va = a + off;
        if (!user_page_readable(va)) {
            return -1;
        }
        uint32_t room = PAGE_SIZE - (va & 0xFFFu);
        uint32_t n = (room < len - off) ? room : (len - off);
        memcpy(d + off, (const void *)(uintptr_t)va, n);
        off += n;
    }
    return 0;
}

/**
 * copy_from_user 的写侧对偶
 *
 * @returns 0 成功；-1 任一页不可写或地址越界
 */
int copy_to_user(void *udst, const void *src, uint32_t len) {
    if (len == 0) {
        return 0;
    }
    uint32_t a = (uint32_t)(uintptr_t)udst;
    if (a < USER_VADDR_BEGIN || a >= USER_SPACE_END ||
        len > USER_SPACE_END - a) {
        return -1;
    }
    const uint8_t *s = (const uint8_t *)src;
    uint32_t off = 0;
    while (off < len) {
        uint32_t va = a + off;
        if (!user_page_writable(va)) {
            return -1;
        }
        uint32_t room = PAGE_SIZE - (va & 0xFFFu);
        uint32_t n = (room < len - off) ? room : (len - off);
        memcpy((void *)(uintptr_t)va, s + off, n);
        off += n;
    }
    return 0;
}

int user_strnlen(const char *src, uint32_t max) {
    return user_str_span(src, max, NULL);
}

static int user_range_walk(uint32_t addr, uint32_t len, int write) {
    if (addr < USER_VADDR_BEGIN || len == 0 ||
        len > USER_SPACE_END - USER_VADDR_BEGIN ||
        addr > USER_SPACE_END - len) {
        return 0;
    }
    uint32_t first = addr & ~0xFFFu;
    uint32_t last = (addr + len - 1) & ~0xFFFu;
    for (uint32_t page = first;; page += PAGE_SIZE) {
        uint64_t *pde = pde_ptr(page);
        if (pde == NULL || !(*pde & PTE_P)) {
            return 0;
        }
        if (*pde & PTE_PS) {
            uint64_t need = PTE_U | (write ? PTE_W : 0);
            if ((*pde & need) != need) {
                return 0;
            }
        } else {
            uint64_t *pte = pte_ptr(page);
            if (!(*pte & PTE_P) || !(*pte & PTE_U)) {
                return 0;
            }
            if (write && !(*pte & PTE_W) &&
                (!(*pte & COW_FLAG) || !page_cow_resolve(page, *pte))) {
                return 0;
            }
        }
        if (page == last) {
            return 1;
        }
    }
}

int user_range_readable(uint32_t addr, uint32_t len) {
    return user_range_walk(addr, len, 0);
}

int user_range_writable(uint32_t addr, uint32_t len) {
    return user_range_walk(addr, len, 1);
}
