#include "drivers/char/pty.h"

#include "arch/cpu.h"
#include "kernel/fs/file.h"
#include "kernel/init/pit/pit.h"
#include "kernel/sched/thread.h"
#include "kernel/signal.h"
#include "lib/str/str.h"

#define PTY_EAGAIN 11
#define PTY_EINTR 4

static struct PTY ptys[NPTY];

struct PTY_REF {
    struct PTY *p;
    int is_master;
    int used;
};
static struct PTY_REF pty_refs[MAX_FILE_OPEN];

static struct PTY_REF *ref_alloc(void) {
    for (uint32_t i = 0; i < MAX_FILE_OPEN; i++) {
        if (!pty_refs[i].used) {
            pty_refs[i].used = 1;
            return &pty_refs[i];
        }
    }
    return 0;
}

int pty_has_priv(const struct FILE *f) {
    return f != 0 && f->dev_priv != 0;
}

int pty_chardev_open(struct FILE *f, uint32_t dev) {
    uint32_t major = dev >> 8;
    if (major == PTY_MASTER_MAJOR) {
        int idx = -1;
        for (int i = 0; i < NPTY; i++) {
            if (!ptys[i].in_use) {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            return -1;
        struct PTY *p = &ptys[idx];
        memset(p, 0, sizeof(*p));
        ioq_init(&p->m2s);
        ioq_init(&p->s2m);
        p->in_use = 1;
        p->master_open = 1;
        p->master_locked = 1;
        p->id = idx;
        p->ws[0] = 25;
        p->ws[1] = 80;
        struct PTY_REF *r = ref_alloc();
        if (!r) {
            p->in_use = 0;
            p->master_open = 0;
            return -1;
        }
        r->p = p;
        r->is_master = 1;
        f->dev_priv = r;
        return 0;
    }
    if (major == PTY_SLAVE_MAJOR) {
        uint32_t minor = dev & 0xffu;
        if (minor >= NPTY)
            return -1;
        struct PTY *p = &ptys[minor];
        if (!p->in_use || !p->master_open || p->master_locked)
            return -1;
        p->slave_open = 1;
        struct PTY_REF *r = ref_alloc();
        if (!r) {
            p->slave_open = 0;
            return -1;
        }
        r->p = p;
        r->is_master = 0;
        f->dev_priv = r;
        return 0;
    }
    return -1;
}

int32_t pty_chardev_read(struct FILE *f, void *buf, uint32_t count) {
    if (!f->dev_priv || count == 0)
        return 0;
    struct PTY_REF *r = (struct PTY_REF *)f->dev_priv;
    struct PTY *p = r->p;
    struct TTY_IOQUEUE *q = r->is_master ? &p->s2m : &p->m2s;
    uint8_t *b = (uint8_t *)buf;
    uint32_t got = 0;
    for (;;) {
        if (got == 0 && (current->signal_pending & ~current->signal_mask)) {
            current->errno = PTY_EINTR;
            return -1;
        }
        uint32_t fl = cpu_eflags();
        cpu_cli();
        uint32_t len = ioq_length(q);
        cpu_set_eflags(fl);
        if (len) {
            uint32_t take = len < (count - got) ? len : (count - got);
            uint32_t f2 = cpu_eflags();
            cpu_cli();
            for (uint32_t i = 0; i < take; i++)
                b[got++] = (uint8_t)ioq_getchar(q);
            cpu_set_eflags(f2);
            if (got >= count)
                return (int32_t)got;
        }
        if (got > 0)
            return (int32_t)got;
        int peer_closed = r->is_master ? !p->slave_open : !p->master_open;
        if (peer_closed)
            return 0;
        if (f->fd_nonblock) {
            current->errno = PTY_EAGAIN;
            return -1;
        }
        mtime_sleep(1);
    }
}

int32_t pty_chardev_write(struct FILE *f, const void *buf, uint32_t count) {
    if (!f->dev_priv)
        return (int32_t)count;
    struct PTY_REF *r = (struct PTY_REF *)f->dev_priv;
    struct PTY *p = r->p;
    struct TTY_IOQUEUE *q = r->is_master ? &p->m2s : &p->s2m;
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t fl = cpu_eflags();
    cpu_cli();
    for (uint32_t i = 0; i < count; i++) {
        while (ioq_full(q)) {
            cpu_set_eflags(fl);
            if (f->fd_nonblock) {
                current->errno = PTY_EAGAIN;
                return -1;
            }
            mtime_sleep(1);
            fl = cpu_eflags();
            cpu_cli();
        }
        ioq_putchar(q, (char)b[i]);
    }
    cpu_set_eflags(fl);
    return (int32_t)count;
}

int pty_chardev_ioctl(struct FILE *f, uint32_t cmd, uint64_t arg) {
    if (!f->dev_priv)
        return -1;
    struct PTY *p = ((struct PTY_REF *)f->dev_priv)->p;
    switch (cmd) {
    case PTY_TIOCGPTN:
        if (arg)
            *(uint32_t *)(uintptr_t)arg = (uint32_t)p->id;
        return 0;
    case PTY_TIOCSPTLCK:
        if (arg)
            p->master_locked = *(const uint32_t *)(uintptr_t)arg ? 1 : 0;
        return 0;
    case PTY_TIOCGWINSZ:
        if (arg)
            memcpy((void *)(uintptr_t)arg, p->ws, 8);
        return 0;
    case PTY_TIOCSWINSZ:
        if (arg) {
            const uint16_t *w = (const uint16_t *)(uintptr_t)arg;
            uint16_t orow = p->ws[0], ocol = p->ws[1];
            memcpy(p->ws, w, 8);
            if (p->pgrp && (p->ws[0] != orow || p->ws[1] != ocol))
                sys_kill(-(int)p->pgrp, SIGWINCH);
        }
        return 0;
    case PTY_TIOCSCTTY:
        p->pgrp = current->pgid ? current->pgid : current->pid;
        return 0;
    case PTY_TIOCGPGRP:
        if (arg)
            *(uint32_t *)(uintptr_t)arg = p->pgrp;
        return 0;
    case PTY_TIOCSPGRP:
        if (arg)
            p->pgrp = *(const uint32_t *)(uintptr_t)arg;
        return 0;
    default:
        return 0;
    }
}

void pty_chardev_close(struct FILE *f) {
    if (!f->dev_priv)
        return;
    struct PTY_REF *r = (struct PTY_REF *)f->dev_priv;
    struct PTY *p = r->p;
    if (r->is_master)
        p->master_open = 0;
    else
        p->slave_open = 0;
    if (!p->master_open && !p->slave_open)
        p->in_use = 0;
    r->used = 0;
    r->p = 0;
    f->dev_priv = 0;
}
