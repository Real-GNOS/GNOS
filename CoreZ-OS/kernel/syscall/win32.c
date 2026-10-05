#include "kernel/syscall/win32.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/tty.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/str/str.h"

struct WIN_OUT {
    char buf[WIN_IO_BUF];
    uint32_t len;
    uint32_t total;
};

struct WIN_VA {
    struct ARCH_REGS *regs;
    int next;
    int ms;
    uint64_t base;
};

typedef int64_t (*win_fn)(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3);

struct WIN_API {
    const char *mod;
    const char *name;
    win_fn fn;
};

static uint32_t win_thunk_base;

static void win_out_flush(struct WIN_OUT *o) {
    if (o->len != 0) {
        TTY.write(o->buf, o->len);
        o->total += o->len;
        o->len = 0;
    }
}

static void win_out_raw(struct WIN_OUT *o, const char *s, uint32_t n) {
    while (n != 0) {
        uint32_t room = WIN_IO_BUF - o->len;
        uint32_t take;
        if (room == 0) {
            win_out_flush(o);
            room = WIN_IO_BUF;
        }
        take = (n < room) ? n : room;
        memcpy(o->buf + o->len, s, take);
        o->len += take;
        s += take;
        n -= take;
    }
}

static void win_out_pad(struct WIN_OUT *o, char c, uint32_t n) {
    while (n != 0) {
        win_out_raw(o, &c, 1);
        n--;
    }
}

static uint32_t win_str_len(const char *s, uint32_t cap) {
    uint32_t done = 0;
    while (done < cap) {
        uint32_t n;
        if (!access_ok(s + done, 1, 0))
            return done;
        n = (uint32_t)user_strnlen(s + done, WIN_STR_CHUNK);
        done += n;
        if (n < WIN_STR_CHUNK)
            return done;
    }
    return done;
}

static void win_out_str(struct WIN_OUT *o, const char *s) {
    uint32_t done = 0;
    while (done < WIN_STR_MAX) {
        uint32_t n;
        if (!access_ok(s + done, 1, 0))
            return;
        n = (uint32_t)user_strnlen(s + done, WIN_STR_CHUNK);
        win_out_raw(o, s + done, n);
        if (n < WIN_STR_CHUNK)
            return;
        done += n;
    }
}

static uint32_t win_num(char *dst, uint64_t v, uint32_t base, int upper) {
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *dig = upper ? upper_digits : lower_digits;
    char tmp[24];
    uint32_t n = 0;
    if (v == 0) {
        dst[0] = '0';
        return 1;
    }
    while (v != 0) {
        tmp[n++] = dig[v % base];
        v /= base;
    }
    for (uint32_t i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];
    return n;
}

static void win_out_field(struct WIN_OUT *o, const char *s, uint32_t n,
                          uint32_t width, int left, int zero) {
    uint32_t pad = (width > n) ? width - n : 0;
    if (!left)
        win_out_pad(o, zero ? '0' : ' ', pad);
    win_out_raw(o, s, n);
    if (left)
        win_out_pad(o, ' ', pad);
}

static int32_t win_va_next(struct WIN_VA *va, uint64_t *out) {
    int i = va->next++;
    uint32_t addr;
    if (va->ms) {
        addr = (uint32_t)va->base + (uint32_t)i * 8u;
        if (!access_ok((const void *)(uintptr_t)addr, 8, 0))
            return -1;
        *out = *(const uint64_t *)(uintptr_t)addr;
        return 0;
    }
    if (i == 0) {
        *out = va->regs->rdx;
        return 0;
    }
    if (i == 1) {
        *out = va->regs->r8;
        return 0;
    }
    if (i == 2) {
        *out = va->regs->r9;
        return 0;
    }
    addr = (uint32_t)va->regs->user_rsp + WIN_STACK_BASE +
           (uint32_t)(i - 3) * 8u;
    if (!access_ok((const void *)(uintptr_t)addr, 8, 0))
        return -1;
    *out = *(const uint64_t *)(uintptr_t)addr;
    return 0;
}

static uint32_t win_vprintf_va(const char *fmt, struct WIN_VA *va) {
    char kfmt[WIN_FMT_MAX];
    char tmp[32];
    struct WIN_OUT out;
    uint32_t flen;
    uint32_t i = 0;

    flen = (uint32_t)user_strnlen(fmt, WIN_FMT_MAX - 1);
    if (!access_ok(fmt, flen + 1, 0))
        return 0;
    copy_from_user(kfmt, fmt, flen);
    kfmt[flen] = 0;
    out.len = 0;
    out.total = 0;

    while (kfmt[i] != 0) {
        char conv;
        int left = 0;
        int zero = 0;
        int wide = 0;
        int neg = 0;
        uint32_t width = 0;
        uint32_t n = 0;
        uint64_t v = 0;

        if (kfmt[i] != '%') {
            win_out_raw(&out, &kfmt[i], 1);
            i++;
            continue;
        }
        i++;
        if (kfmt[i] == '%') {
            win_out_raw(&out, "%", 1);
            i++;
            continue;
        }
        for (;;) {
            if (kfmt[i] == '-') {
                left = 1;
                i++;
                continue;
            }
            if (kfmt[i] == '0') {
                zero = 1;
                i++;
                continue;
            }
            if (kfmt[i] == '+' || kfmt[i] == ' ' || kfmt[i] == '#') {
                i++;
                continue;
            }
            break;
        }
        while (kfmt[i] >= '0' && kfmt[i] <= '9') {
            width = width * 10u + (uint32_t)(kfmt[i] - '0');
            i++;
        }
        while (kfmt[i] == 'l' || kfmt[i] == 'h' || kfmt[i] == 'z' ||
               kfmt[i] == 'j' || kfmt[i] == 't') {
            if (kfmt[i] == 'l')
                wide = 1;
            i++;
        }
        conv = kfmt[i];
        if (conv == 0)
            break;
        i++;
        if (win_va_next(va, &v) != 0)
            break;
        switch (conv) {
        case 's': {
            const char *s = (const char *)(uintptr_t)v;
            uint32_t slen = win_str_len(s, WIN_STR_MAX);
            uint32_t pad = (width > slen) ? width - slen : 0;
            if (!left)
                win_out_pad(&out, ' ', pad);
            win_out_str(&out, s);
            if (left)
                win_out_pad(&out, ' ', pad);
            continue;
        }
        case 'd':
        case 'i': {
            int64_t sv = wide ? (int64_t)v : (int64_t)(int32_t)(uint32_t)v;
            uint64_t mag = (sv < 0) ? (uint64_t)(-sv) : (uint64_t)sv;
            if (sv < 0) {
                neg = 1;
                tmp[n++] = '-';
            }
            n += win_num(tmp + n, mag, 10, 0);
            break;
        }
        case 'u':
            n = win_num(tmp, wide ? v : (uint32_t)v, 10, 0);
            break;
        case 'x':
            n = win_num(tmp, wide ? v : (uint32_t)v, 16, 0);
            break;
        case 'X':
            n = win_num(tmp, wide ? v : (uint32_t)v, 16, 1);
            break;
        case 'p':
            tmp[0] = '0';
            tmp[1] = 'x';
            n = 2 + win_num(tmp + 2, v, 16, 1);
            break;
        case 'c':
            tmp[0] = (char)(uint32_t)v;
            n = 1;
            break;
        default:
            tmp[0] = '%';
            tmp[1] = conv;
            n = 2;
            break;
        }
        win_out_field(&out, tmp, n, width, left, zero && !neg);
    }
    win_out_flush(&out);
    return out.total;
}

static uint32_t win_vprintf(struct ARCH_REGS *r, const char *fmt) {
    struct WIN_VA va;
    va.regs = r;
    va.next = 0;
    va.ms = 0;
    va.base = 0;
    return win_vprintf_va(fmt, &va);
}

static uint64_t win_rt_ptr(uint32_t off) {
    if (current->win_rt == 0)
        return 0;
    return (uint64_t)(current->win_rt + off);
}

uint64_t win_heap_alloc(uint32_t need, int zero) {
    struct TASK *cur = current;
    uint32_t base;
    uint32_t p;
    uint32_t end;
    if (need == 0)
        need = 1;
    base = (cur->user_brk != 0)
               ? cur->user_brk
               : ((cur->brk_base != 0) ? cur->brk_base : USER_HEAP_BASE);
    p = (base + 0xfu) & ~0xfu;
    end = p + need;
    if (end < p || end > USER_HEAP_LIMIT)
        return 0;
    for (uint32_t pg = p & ~(PAGE_SIZE - 1);
         pg < ((end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)); pg += PAGE_SIZE) {
        if (!page_is_mapped(pg) && get_a_page(pg) == 0)
            return 0;
    }
    cur->user_brk = end;
    if (zero)
        memset((void *)(uintptr_t)p, 0, need);
    return (uint64_t)p;
}

int32_t win_user_name(char *dst, uint32_t cap, uint64_t uptr) {
    uint32_t len;
    if (uptr == 0)
        return -1;
    len = (uint32_t)user_strnlen((const char *)(uintptr_t)uptr, cap - 1);
    if (len == 0 || !access_ok((const void *)(uintptr_t)uptr, len + 1, 0))
        return -1;
    copy_from_user(dst, (const void *)(uintptr_t)uptr, len);
    dst[len] = 0;
    return 0;
}

static int64_t win_null(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_write_file(struct ARCH_REGS *r, uint64_t handle,
                              uint64_t buf, uint64_t n, uint64_t written) {
    (void)r;
    (void)handle;
    if (n != 0 && !access_ok((const void *)(uintptr_t)buf, (size_t)n, 0))
        return 0;
    uint32_t w = (uint32_t)TTY.write((const char *)(uintptr_t)buf, (uint32_t)n);
    if (written != 0 && access_ok((const void *)(uintptr_t)written, 4, 1))
        *(uint32_t *)(uintptr_t)written = w;
    return 1;
}

static int64_t win_get_std_handle(struct ARCH_REGS *r, uint64_t which,
                                  uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(int32_t)(uint32_t)which;
}

static int64_t win_exit_process(struct ARCH_REGS *r, uint64_t code, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    sys_exit((int32_t)code);
    return 0;
}

static int64_t win_get_last_error(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_set_last_error(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_sleep(struct ARCH_REGS *r, uint64_t ms, uint64_t a1,
                         uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    mtime_sleep_interruptible((uint32_t)ms);
    return 0;
}

static int64_t win_get_tick_count(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)((uint64_t)tick * 1000u / PIT_HZ);
}

static int64_t win_get_module_handle_a(struct ARCH_REGS *r, uint64_t name,
                                       uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)r;
    (void)name;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(uint64_t)current->win_base;
}

static int64_t win_get_proc_address(struct ARCH_REGS *r, uint64_t mod,
                                    uint64_t name, uint64_t a2, uint64_t a3) {
    char kname[WIN_NAME_MAX];
    (void)r;
    (void)mod;
    (void)a2;
    (void)a3;
    if (win_user_name(kname, sizeof(kname), name) != 0)
        return 0;
    return (int64_t)(uint64_t)win32_lookup(0, kname);
}

static int64_t win_load_library_a(struct ARCH_REGS *r, uint64_t name,
                                  uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)r;
    (void)name;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(uint64_t)current->win_base;
}

static int64_t win_free_library(struct ARCH_REGS *r, uint64_t mod, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    (void)r;
    (void)mod;
    (void)a1;
    (void)a2;
    (void)a3;
    return 1;
}

static int64_t win_virtual_protect(struct ARCH_REGS *r, uint64_t addr,
                                   uint64_t size, uint64_t prot,
                                   uint64_t old) {
    (void)r;
    (void)addr;
    (void)size;
    (void)prot;
    if (old != 0 && access_ok((const void *)(uintptr_t)old, 4, 1))
        *(uint32_t *)(uintptr_t)old = WIN_PAGE_EXECUTE_READWRITE;
    return 1;
}

static int64_t win_virtual_query(struct ARCH_REGS *r, uint64_t addr,
                                 uint64_t buf, uint64_t len, uint64_t a3) {
    (void)r;
    (void)addr;
    (void)a3;
    if (len != 0 && access_ok((const void *)(uintptr_t)buf, (size_t)len, 1)) {
        memset((void *)(uintptr_t)buf, 0, (size_t)len);
        if (len >= 0x20u)
            *(uint64_t *)(uintptr_t)(buf + 0x18u) = len;
    }
    return WIN_MEMORY_BASIC_SIZE;
}

static int64_t win_tls_get_value(struct ARCH_REGS *r, uint64_t idx, uint64_t a1,
                                 uint64_t a2, uint64_t a3) {
    (void)r;
    (void)idx;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_tls_set_value(struct ARCH_REGS *r, uint64_t idx, uint64_t v,
                                 uint64_t a2, uint64_t a3) {
    (void)r;
    (void)idx;
    (void)v;
    (void)a2;
    (void)a3;
    return 1;
}

static int64_t win_section_noop(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_puts(struct ARCH_REGS *r, uint64_t s, uint64_t a1,
                        uint64_t a2, uint64_t a3) {
    struct WIN_OUT out;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    out.len = 0;
    out.total = 0;
    win_out_str(&out, (const char *)(uintptr_t)s);
    win_out_raw(&out, "\n", 1);
    win_out_flush(&out);
    return 0;
}

static int64_t win_putchar(struct ARCH_REGS *r, uint64_t c, uint64_t a1,
                           uint64_t a2, uint64_t a3) {
    char ch = (char)(uint32_t)c;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    TTY.write(&ch, 1);
    return (int64_t)(uint8_t)ch;
}

static int64_t win_printf(struct ARCH_REGS *r, uint64_t fmt, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_vprintf(r, (const char *)(uintptr_t)fmt);
}

static int64_t win_strlen(struct ARCH_REGS *r, uint64_t s, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)(uint64_t)win_str_len((const char *)(uintptr_t)s, 0x10000u);
}

static int64_t win_strcmp(struct ARCH_REGS *r, uint64_t a, uint64_t b,
                          uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a2;
    (void)a3;
    for (uint32_t i = 0; i < WIN_STR_MAX; i++) {
        uint8_t ca;
        uint8_t cb;
        if (!access_ok((const void *)(uintptr_t)(a + i), 1, 0) ||
            !access_ok((const void *)(uintptr_t)(b + i), 1, 0))
            return 0;
        ca = *(const uint8_t *)(uintptr_t)(a + i);
        cb = *(const uint8_t *)(uintptr_t)(b + i);
        if (ca != cb)
            return (int64_t)(int32_t)(ca - cb);
        if (ca == 0)
            return 0;
    }
    return 0;
}

static int64_t win_strncmp(struct ARCH_REGS *r, uint64_t a, uint64_t b,
                           uint64_t n, uint64_t a3) {
    (void)r;
    (void)a3;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t ca;
        uint8_t cb;
        if (!access_ok((const void *)(uintptr_t)(a + i), 1, 0) ||
            !access_ok((const void *)(uintptr_t)(b + i), 1, 0))
            return 0;
        ca = *(const uint8_t *)(uintptr_t)(a + i);
        cb = *(const uint8_t *)(uintptr_t)(b + i);
        if (ca != cb)
            return (int64_t)(int32_t)(ca - cb);
        if (ca == 0)
            return 0;
    }
    return 0;
}

static int64_t win_memcpy(struct ARCH_REGS *r, uint64_t dst, uint64_t src,
                          uint64_t n, uint64_t a3) {
    (void)r;
    (void)a3;
    if (n != 0 && access_ok((const void *)(uintptr_t)dst, (size_t)n, 1) &&
        access_ok((const void *)(uintptr_t)src, (size_t)n, 0))
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
    return (int64_t)dst;
}

static int64_t win_memset(struct ARCH_REGS *r, uint64_t dst, uint64_t c,
                          uint64_t n, uint64_t a3) {
    (void)r;
    (void)a3;
    if (n != 0 && access_ok((const void *)(uintptr_t)dst, (size_t)n, 1))
        memset((void *)(uintptr_t)dst, (int32_t)c, (size_t)n);
    return (int64_t)dst;
}

static int64_t win_crt_vfprintf(struct ARCH_REGS *r, uint64_t options,
                                uint64_t stream, uint64_t fmt, uint64_t locale) {
    struct WIN_VA va;
    uint32_t addr;
    (void)options;
    (void)stream;
    (void)locale;
    addr = (uint32_t)r->user_rsp + WIN_STACK_BASE;
    if (!access_ok((const void *)(uintptr_t)addr, 8, 0))
        return 0;
    va.regs = r;
    va.next = 0;
    va.ms = 1;
    va.base = *(const uint64_t *)(uintptr_t)addr;
    return (int64_t)win_vprintf_va((const char *)(uintptr_t)fmt, &va);
}

static int64_t win_p___argc(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_rt_ptr(WIN_RT_ARGC);
}

static int64_t win_p___argv(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_rt_ptr(WIN_RT_ARGV);
}

static int64_t win_p__environ(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_rt_ptr(WIN_RT_ENVP);
}

static int64_t win_p__fmode(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_rt_ptr(WIN_RT_FMODE);
}

static int64_t win_p__commode(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_rt_ptr(WIN_RT_COMMODE);
}

static int64_t win_acrt_iob_func(struct ARCH_REGS *r, uint64_t idx, uint64_t a1,
                                 uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)win_rt_ptr(WIN_RT_FILE +
                               (uint32_t)idx * WIN_RT_FILE_STRIDE);
}

static int64_t win_malloc(struct ARCH_REGS *r, uint64_t n, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    if (n > 0x7fffffffu)
        return 0;
    return (int64_t)win_heap_alloc((uint32_t)n, 0);
}

static int64_t win_calloc(struct ARCH_REGS *r, uint64_t n, uint64_t sz,
                          uint64_t a2, uint64_t a3) {
    uint64_t total = n * sz;
    (void)r;
    (void)a2;
    (void)a3;
    if (sz != 0 && total / sz != n)
        return 0;
    if (total > 0x7fffffffu)
        return 0;
    return (int64_t)win_heap_alloc((uint32_t)total, 1);
}

static int64_t win_free(struct ARCH_REGS *r, uint64_t p, uint64_t a1,
                        uint64_t a2, uint64_t a3) {
    (void)r;
    (void)p;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

static int64_t win_seh_handler(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                               uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 1;
}

static const struct WIN_API win_api_table[] = {
    {"kernel32", "WriteFile", win_write_file},
    {"kernel32", "GetStdHandle", win_get_std_handle},
    {"kernel32", "ExitProcess", win_exit_process},
    {"kernel32", "GetLastError", win_get_last_error},
    {"kernel32", "SetLastError", win_set_last_error},
    {"kernel32", "Sleep", win_sleep},
    {"kernel32", "GetTickCount", win_get_tick_count},
    {"kernel32", "GetModuleHandleA", win_get_module_handle_a},
    {"kernel32", "GetProcAddress", win_get_proc_address},
    {"kernel32", "LoadLibraryA", win_load_library_a},
    {"kernel32", "FreeLibrary", win_free_library},
    {"kernel32", "VirtualProtect", win_virtual_protect},
    {"kernel32", "VirtualQuery", win_virtual_query},
    {"kernel32", "TlsGetValue", win_tls_get_value},
    {"kernel32", "TlsSetValue", win_tls_set_value},
    {"kernel32", "InitializeCriticalSection", win_section_noop},
    {"kernel32", "EnterCriticalSection", win_section_noop},
    {"kernel32", "LeaveCriticalSection", win_section_noop},
    {"kernel32", "DeleteCriticalSection", win_section_noop},
    {"kernel32", "SetUnhandledExceptionFilter", win_section_noop},
    {"msvcrt", "printf", win_printf},
    {"msvcrt", "puts", win_puts},
    {"msvcrt", "putchar", win_putchar},
    {"msvcrt", "strlen", win_strlen},
    {"msvcrt", "strcmp", win_strcmp},
    {"msvcrt", "strncmp", win_strncmp},
    {"msvcrt", "memcpy", win_memcpy},
    {"msvcrt", "memset", win_memset},
    {"msvcrt", "exit", win_exit_process},
    {"msvcrt", "_exit", win_exit_process},
    {"msvcrt", "abort", win_exit_process},
    {"api-ms-win-crt-environment-l1-1-0", "__p__environ", win_p__environ},
    {"api-ms-win-crt-heap-l1-1-0", "_set_new_mode", win_null},
    {"api-ms-win-crt-heap-l1-1-0", "calloc", win_calloc},
    {"api-ms-win-crt-heap-l1-1-0", "free", win_free},
    {"api-ms-win-crt-heap-l1-1-0", "malloc", win_malloc},
    {"api-ms-win-crt-locale-l1-1-0", "_configthreadlocale", win_null},
    {"api-ms-win-crt-math-l1-1-0", "__setusermatherr", win_null},
    {"api-ms-win-crt-private-l1-1-0", "__C_specific_handler", win_seh_handler},
    {"api-ms-win-crt-private-l1-1-0", "memcpy", win_memcpy},
    {"api-ms-win-crt-runtime-l1-1-0", "_set_app_type", win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "__p___argc", win_p___argc},
    {"api-ms-win-crt-runtime-l1-1-0", "__p___argv", win_p___argv},
    {"api-ms-win-crt-runtime-l1-1-0", "_cexit", win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "_configure_narrow_argv", win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "_crt_atexit", win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "_exit", win_exit_process},
    {"api-ms-win-crt-runtime-l1-1-0", "_initialize_narrow_environment",
     win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "_initterm", win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "_initterm_e", win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "_set_invalid_parameter_handler",
     win_null},
    {"api-ms-win-crt-runtime-l1-1-0", "abort", win_exit_process},
    {"api-ms-win-crt-runtime-l1-1-0", "exit", win_exit_process},
    {"api-ms-win-crt-runtime-l1-1-0", "signal", win_null},
    {"api-ms-win-crt-stdio-l1-1-0", "__acrt_iob_func", win_acrt_iob_func},
    {"api-ms-win-crt-stdio-l1-1-0", "__p__commode", win_p__commode},
    {"api-ms-win-crt-stdio-l1-1-0", "__p__fmode", win_p__fmode},
    {"api-ms-win-crt-stdio-l1-1-0", "__stdio_common_vfprintf",
     win_crt_vfprintf},
    {"api-ms-win-crt-stdio-l1-1-0", "fflush", win_null},
    {"api-ms-win-crt-stdio-l1-1-0", "setvbuf", win_null},
    {"api-ms-win-crt-string-l1-1-0", "strlen", win_strlen},
    {"api-ms-win-crt-string-l1-1-0", "strncmp", win_strncmp},
    {"user32", "RegisterClassA", w32_register_class_a},
    {"user32", "CreateWindowExA", w32_create_window_ex_a},
    {"user32", "ShowWindow", w32_show_window},
    {"user32", "UpdateWindow", w32_update_window},
    {"user32", "GetMessageA", w32_get_message_a},
    {"user32", "PeekMessageA", w32_peek_message_a},
    {"user32", "TranslateMessage", w32_translate_message},
    {"user32", "DispatchMessageA", w32_dispatch_message_a},
    {"user32", "DefWindowProcA", w32_def_window_proc_a},
    {"user32", "PostQuitMessage", w32_post_quit_message},
    {"user32", "PostMessageA", w32_post_message_a},
    {"user32", "DestroyWindow", w32_destroy_window},
    {"user32", "GetDC", w32_get_dc},
    {"user32", "ReleaseDC", w32_release_dc},
    {"user32", "BeginPaint", w32_begin_paint},
    {"user32", "EndPaint", w32_end_paint},
    {"user32", "GetClientRect", w32_get_client_rect},
    {"user32", "InvalidateRect", w32_invalidate_rect},
    {"user32", "LoadCursorA", w32_load_cursor_a},
    {"user32", "LoadIconA", w32_load_icon_a},
    {"user32", "MessageBoxA", w32_message_box_a},
    {"user32", "SetWindowTextA", w32_set_window_text_a},
    {"user32", "GetSystemMetrics", w32_get_system_metrics},
    {"user32", "MessageBeep", w32_message_beep},
    {"gdi32", "CreateCompatibleDC", w32_create_compatible_dc},
    {"gdi32", "CreateCompatibleBitmap", w32_create_compatible_bitmap},
    {"gdi32", "CreateDIBSection", w32_create_dib_section},
    {"gdi32", "SelectObject", w32_select_object},
    {"gdi32", "DeleteObject", w32_delete_object},
    {"gdi32", "DeleteDC", w32_delete_dc},
    {"gdi32", "BitBlt", w32_bit_blt},
    {"gdi32", "StretchBlt", w32_stretch_blt},
    {"gdi32", "PatBlt", w32_pat_blt},
    {"gdi32", "Rectangle", w32_rectangle},
    {"gdi32", "Ellipse", w32_ellipse},
    {"gdi32", "MoveToEx", w32_move_to_ex},
    {"gdi32", "LineTo", w32_line_to},
    {"gdi32", "TextOutA", w32_text_out_a},
    {"gdi32", "DrawTextA", w32_draw_text_a},
    {"gdi32", "SetTextColor", w32_set_text_color},
    {"gdi32", "SetBkColor", w32_set_bk_color},
    {"gdi32", "SetBkMode", w32_set_bk_mode},
    {"gdi32", "CreateSolidBrush", w32_create_solid_brush},
    {"gdi32", "CreatePen", w32_create_pen},
    {"gdi32", "GetStockObject", w32_get_stock_object},
    {"gdi32", "FillRect", w32_fill_rect},
    {"gdi32", "FrameRect", w32_frame_rect},
    {"gdi32", "SetPixel", w32_set_pixel},
    {"gdi32", "GetDeviceCaps", w32_get_device_caps},
    {"", "", win_null},
};

#define WIN_API_COUNT ((uint32_t)(sizeof(win_api_table) / sizeof(win_api_table[0])))

_Static_assert(WIN_API_COUNT + 4u <= WIN_THUNK_SLOTS,
               "win32 api table exceeds thunk page");

void win32_thunk_init(uint32_t base) {
    uint32_t body = base + WIN_API_COUNT * WIN_THUNK_SIZE;
    uint64_t body_addr = (uint64_t)body;
    uint32_t dnr = WIN32_SYSCALL_BASE;
    win_thunk_base = base;
    for (uint32_t i = 0; i < WIN_API_COUNT; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)(base + i * WIN_THUNK_SIZE);
        uint32_t nr = WIN32_SYSCALL_BASE + i;
        if (win_api_table[i].fn == w32_dispatch_message_a) {
            dnr = nr;
            p[0] = 0xff;
            p[1] = 0x25;
            p[2] = 0x00;
            p[3] = 0x00;
            p[4] = 0x00;
            p[5] = 0x00;
            for (uint32_t k = 0; k < 8; k++)
                p[6 + k] = (uint8_t)((body_addr >> (8u * k)) & 0xffu);
            for (uint32_t k = 14; k < WIN_THUNK_SIZE; k++)
                p[k] = 0xcc;
            continue;
        }
        p[0] = 0x49;
        p[1] = 0x89;
        p[2] = 0xca;
        p[3] = 0xb8;
        p[4] = (uint8_t)(nr & 0xffu);
        p[5] = (uint8_t)((nr >> 8) & 0xffu);
        p[6] = (uint8_t)((nr >> 16) & 0xffu);
        p[7] = (uint8_t)((nr >> 24) & 0xffu);
        p[8] = 0x0f;
        p[9] = 0x05;
        p[10] = 0xc3;
        for (uint32_t k = 11; k < WIN_THUNK_SIZE; k++)
            p[k] = 0xcc;
    }
    uint8_t *b = (uint8_t *)(uintptr_t)body;
    static const uint8_t body_code[] = {
        0x49, 0x89, 0xca, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48,
        0x85, 0xc0, 0x74, 0x19, 0x49, 0x8b, 0x0a, 0x49, 0x8b, 0x52, 0x08,
        0x4d, 0x8b, 0x42, 0x10, 0x4d, 0x8b, 0x4a, 0x18, 0x48, 0x83, 0xec,
        0x28, 0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3,
    };
    for (uint32_t k = 0; k < sizeof(body_code); k++)
        b[k] = body_code[k];
    b[4] = (uint8_t)(dnr & 0xffu);
    b[5] = (uint8_t)((dnr >> 8) & 0xffu);
    b[6] = (uint8_t)((dnr >> 16) & 0xffu);
    b[7] = (uint8_t)((dnr >> 24) & 0xffu);
    for (uint32_t k = (uint32_t)sizeof(body_code); k < 3u * WIN_THUNK_SIZE; k++)
        b[k] = 0xcc;
}

static int win_char_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z')
        a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
        b = (char)(b - 'A' + 'a');
    return a == b;
}

static int win_mod_eq(const char *a, const char *b) {
    if (a == 0 || b == 0)
        return 0;
    while (*a != 0 && *b != 0 && *a != '.' && *b != '.') {
        if (!win_char_eq(*a, *b))
            return 0;
        a++;
        b++;
    }
    return (*a == 0 || *a == '.') && (*b == 0 || *b == '.');
}

uint32_t win32_lookup(const char *mod, const char *name) {
    uint32_t hit = 0;
    for (uint32_t i = 0; i < WIN_API_COUNT; i++) {
        if (strcmp(name, win_api_table[i].name) != 0)
            continue;
        if (win_mod_eq(mod, win_api_table[i].mod))
            return win_thunk_base + i * WIN_THUNK_SIZE;
        if (hit == 0)
            hit = win_thunk_base + i * WIN_THUNK_SIZE;
    }
    return hit;
}

uint32_t win32_resolve(const char *mod, const char *name) {
    uint32_t t = win32_lookup(mod, name);
    if (t != 0)
        return t;
    kprintf("[pe] unresolved import %s!%s\n", mod != 0 ? mod : "?", name);
    return win_thunk_base + (WIN_API_COUNT - 1) * WIN_THUNK_SIZE;
}

int64_t win32_handler(struct ARCH_REGS *r) {
    uint32_t idx = (uint32_t)r->eax - WIN32_SYSCALL_BASE;
    if (idx >= WIN_API_COUNT)
        return 0;
    return win_api_table[idx].fn(r, r->r10, r->rdx, r->r8, r->r9);
}