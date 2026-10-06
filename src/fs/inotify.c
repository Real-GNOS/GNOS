/* SPDX-License-Identifier: GPL-2.0 */
/*
 * inotify.c — inotify_init1(294)/inotify_init(253)/add_watch(254)/
 * rm_watch(255), the filesystem-watching fd. (GPLv2)
 *
 * The real thing: a watch set bound to absolute paths, a per-instance
 * event queue, and VFS mutation hooks (see inotify_vfs.c) that append
 * IN_CREATE/IN_DELETE/IN_MODIFY/... records in the Linux read(2) layout
 * (wd, mask, cookie, len, name[]).  read() drains oldest-first; poll()
 * reports readable exactly while the queue is non-empty; a full queue
 * collapses the next event into one IN_Q_OVERFLOW record on wd=-1.
 *
 * Watches match by absolute-path prefix: a directory watch fires for
 * changes to the directory itself and to every path under it.  That is
 * the observable core of inotify for the single-flat-tree VFS here --
 * there are no inode generations to follow across renames, so a rename
 * is reported as MOVED_FROM on the old parent and MOVED_TO on the new
 * one, sharing a cookie, which is how Linux lets watchers correlate.
 *
 * Instance fds are anonymous nodes (VFS_ANON) freed through ops->release
 * on the last close, like memfd/eventfd/epoll.  The instance table is a
 * fixed array scanned by the fire path; the fire path runs in the
 * mutating syscall's caller, so no locking beyond the interrupt-off
 * window the kernel already provides is taken.
 */
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "heap.h"
#include "proc.h"
#include "kstring.h"
#include "syscall.h"
#include "mutex.h"
#include "inotify.h"

/* Event bits and init flags come from inotify.h (shared with the VFS
 * hooks).  Only the implementation-private knobs live here. */

#define IN_MASK_CREATE 0x10000000
#define IN_MASK_ADD    0x20000000
#define IN_ONESHOT     0x80000000

/* Masks a watch may legitimately ask for; anything else is EINVAL. */
#define IN_WATCHABLE_MASK 0x0000ffff

#define INOTIFY_MAX_WATCHES 128
#define INOTIFY_MAX_INSTANCES 16
#define INOTIFY_Q_BYTES 4096

/* One queued event, kernel-side: the read(2) layout is wd/mask/cookie/
 * len/name[] with name len-padded, so keep the exact record and copy it
 * out verbatim. */
typedef struct inotify_rec {
    struct inotify_rec *next;
    int32_t  wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;   /* name length, 0 when unnamed */
    uint32_t off;   /* offset of name within the record's data */
    char     name[INOTIFY_Q_BYTES >= 256 ? 224 : 32]; /* keeps rec <= 256B */
} inotify_rec_t;

/* A watch: an absolute path prefix plus the mask the caller asked for. */
typedef struct {
    int      used;
    int      wd;
    uint32_t mask;
    char     path[GNUOS_PATH_MAX];
} inotify_watch_t;

typedef struct inotify_inst {
    struct inotify_inst *next;
    inotify_watch_t watches[INOTIFY_MAX_WATCHES];
    inotify_rec_t  *q_head;
    inotify_rec_t  *q_tail;
    uint32_t        q_bytes; /* records queued, vs the byte cap */
    uint32_t        q_count;
    int             next_wd;
    int             nonblock;
} inotify_inst_t;

static inotify_inst_t *g_insts[INOTIFY_MAX_INSTANCES];

/* One mutex serialises everything the instances share: the instance
 * table, each queue's head/tail, and the watch arrays.  A static,
 * zero-initialised mutex_t is already unlocked (slot.v == 0, owner and
 * waiters NULL), so no init hook is needed -- the BKL cannot protect
 * this because the fire path runs deep inside VFS syscalls that
 * themselves sleep, and the read path blocks on the queue. */
static mutex_t g_inotify_mutex = {0};

static int inst_of(int fd, inotify_inst_t **out)
{
    int h = fd_handle(fd);
    if (h < 0)
        return -E_BADF;
    const vfs_node_t *n = vfs_file_node(h);
    if (!n || n->kind != VFS_ANON || !n->ops || !n->priv)
        return -E_INVAL;
    *out = (inotify_inst_t *)(uintptr_t)n->priv;
    return 0;
}

static void inotify_release(vfs_node_t *n)
{
    inotify_inst_t *inst = (inotify_inst_t *)n->priv;
    if (!inst)
        return;
    n->priv = NULL;
    /* The instance may be sitting in the shared table with queued
     * records another process's fire is appending to right now; the
     * table edit and the queue teardown happen under the same mutex
     * the fire path holds, so no record is dropped or double-freed. */
    mutex_lock(&g_inotify_mutex);
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++)
        if (g_insts[i] == inst)
            g_insts[i] = NULL;
    mutex_unlock(&g_inotify_mutex);
    inotify_rec_t *r = inst->q_head;
    while (r) {
        inotify_rec_t *nx = r->next;
        kfree(r);
        r = nx;
    }
    kfree(inst);
}

/* ---- the fire path (called from the VFS hooks) ------------------------ */

/* Append one record to an instance.  A full queue collapses the event
 * into a single IN_Q_OVERFLOW on wd=-1, exactly Linux's rule, and an
 * already-overflowed queue stays that way instead of spamming. */
static void inst_enqueue(inotify_inst_t *inst, int wd, uint32_t mask,
                         uint32_t cookie, const char *name)
{
    static int overflowed_pending;

    if (!inst)
        return;
    uint32_t nlen = name ? (uint32_t)strlen(name) + 1 : 0;
    if (nlen > sizeof(((inotify_rec_t *)0)->name))
        nlen = sizeof(((inotify_rec_t *)0)->name);

    if (inst->q_count >= INOTIFY_Q_BYTES / 16 ||
        inst->q_bytes + sizeof(inotify_rec_t) > INOTIFY_Q_BYTES) {
        if (!overflowed_pending) {
            overflowed_pending = 1;
            inotify_rec_t *r = kmalloc(sizeof(inotify_rec_t));
            if (!r)
                return;
            memset(r, 0, sizeof *r);
            r->wd   = -1;
            r->mask = IN_Q_OVERFLOW;
            r->len  = 0;
            inst->q_bytes += sizeof(inotify_rec_t);
            inst->q_count++;
            if (inst->q_tail)
                inst->q_tail->next = r;
            else
                inst->q_head = r;
            inst->q_tail = r;
        }
        return;
    }
    overflowed_pending = 0;

    inotify_rec_t *r = kmalloc(sizeof(inotify_rec_t));
    if (!r)
        return;
    memset(r, 0, sizeof *r);
    r->wd    = wd;
    r->mask  = mask;
    r->cookie = cookie;
    r->len   = nlen;
    if (nlen)
        memcpy(r->name, name, nlen);
    inst->q_bytes += sizeof(inotify_rec_t);
    inst->q_count++;
    if (inst->q_tail)
        inst->q_tail->next = r;
    else
        inst->q_head = r;
    inst->q_tail = r;
}

void inotify_event_fire(const char *path, uint32_t mask, uint32_t cookie)
{
    if (!path || path[0] != '/')
        return;
    size_t plen = strlen(path);

    mutex_lock(&g_inotify_mutex);
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        inotify_inst_t *inst = g_insts[i];
        if (!inst)
            continue;
        for (int w = 0; w < INOTIFY_MAX_WATCHES; w++) {
            if (!inst->watches[w].used)
                continue;
            const char *wpath = inst->watches[w].path;
            size_t      wlen  = strlen(wpath);
            /* A watch on a directory fires for the directory itself and
             * everything below it; a file watch fires only on the exact
             * path. */
            int match = (plen == wlen && memcmp(path, wpath, wlen) == 0) ||
                        (plen > wlen && wpath[wlen - 1] == '/' &&
                         memcmp(path, wpath, wlen) == 0) ||
                        (plen > wlen && memcmp(path, wpath, wlen) == 0 &&
                         path[wlen] == '/');
            if (!match)
                continue;
            uint32_t wmask = inst->watches[w].mask;
            if (!(mask & wmask))
                continue;
            /* IN_ISDIR: the hooks that know say so in the mask already. */
            inst_enqueue(inst, inst->watches[w].wd, mask, cookie,
                         plen > wlen ? path + wlen + (path[wlen] == '/' ? 1 : 0)
                                     : NULL);
            /* IN_ONESHOT: one event, then the watch removes itself. */
            if (wmask & IN_ONESHOT) {
                inst->watches[w].used = 0;
            }
        }
    }
    mutex_unlock(&g_inotify_mutex);
}

/* ---- read(2): drain the queue ----------------------------------------- */

static int32_t inotify_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    if (!buf || !len)
        return -E_INVAL;
    inotify_inst_t *inst = (inotify_inst_t *)n->priv;
    if (!inst)
        return -E_INVAL;

    /* The queue, the watch arrays and the instance table are all under
     * the one mutex: a fire from another process's write() must not
     * interleave with the drain.  Taking it here also means a reader
     * parked behind a mutator's fire simply queues on the mutex. */
    mutex_lock(&g_inotify_mutex);
    if (!inst->q_head) {
        mutex_unlock(&g_inotify_mutex);
        if (inst->nonblock)
            return -E_AGAIN;
        /* The queue can only fill from a syscall this process is not in;
         * without a sleep channel wired to the fire path there is nothing
         * to wait on, so a blocking read on an empty queue returns EAGAIN
         * rather than hanging the caller forever. */
        return -E_AGAIN;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    while (inst->q_head) {
        inotify_rec_t *r   = inst->q_head;
        uint32_t       reclen = sizeof(inotify_rec_t) - sizeof(r->name) + r->len;
        if (got && got + reclen > len)
            break; /* no room for another whole event */
        if (!got && reclen > len) {
            mutex_unlock(&g_inotify_mutex);
            return -E_INVAL; /* not even one event fits */
        }
        memcpy(out + got, &r->wd, sizeof(int32_t));
        memcpy(out + got + 4, &r->mask, sizeof(uint32_t));
        memcpy(out + got + 8, &r->cookie, sizeof(uint32_t));
        memcpy(out + got + 12, &r->len, sizeof(uint32_t));
        if (r->len)
            memcpy(out + got + 16, r->name, r->len);
        got += reclen;

        inst->q_head = r->next;
        if (!inst->q_head)
            inst->q_tail = NULL;
        inst->q_bytes -= sizeof(inotify_rec_t);
        inst->q_count--;
        kfree(r);
    }
    mutex_unlock(&g_inotify_mutex);
    return (int32_t)got;
}

/* poll(2)/epoll readiness: readable iff the queue holds events.  POLLIN/
 * POLLRDNORM are the poll(2) bits from the UAPI header. */
#define POLLIN      0x001
#define POLLRDNORM  0x040

static int inotify_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    inotify_inst_t *inst = (inotify_inst_t *)n->priv;
    int16_t         r    = 0;
    if (inst && inst->q_head && (events & (POLLIN | POLLRDNORM)))
        r |= POLLIN | POLLRDNORM;
    *revents = r;
    return 0;
}

static const vfs_ops_t g_inotify_ops = {
    .read    = inotify_read,
    .poll    = inotify_poll,
    .release = inotify_release,
};

/* ---- syscalls ---------------------------------------------------------- */

static int instance_slot(inotify_inst_t *inst)
{
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        if (!g_insts[i]) {
            g_insts[i] = inst;
            return 0;
        }
    }
    return -1;
}

int64_t sys_inotify_init1(uint64_t flags)
{
    if (flags & ~(uint64_t)(IN_CLOEXEC | IN_NONBLOCK))
        return -E_INVAL;

    inotify_inst_t *inst = kmalloc(sizeof(inotify_inst_t));
    if (!inst)
        return -E_NOMEM;
    memset(inst, 0, sizeof *inst);
    inst->next_wd  = 1;
    inst->nonblock = (flags & IN_NONBLOCK) ? 1 : 0;

    mutex_lock(&g_inotify_mutex);
    int slot = instance_slot(inst);
    mutex_unlock(&g_inotify_mutex);
    if (slot < 0) {
        kfree(inst);
        return -E_NOSPC;
    }

    int h = vfs_anon_open(VFS_ANON, &g_inotify_ops, inst, O_RDWR);
    if (h < 0) {
        for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++)
            if (g_insts[i] == inst)
                g_insts[i] = NULL;
        kfree(inst);
        return h;
    }
    int fd = fd_alloc(proc_current(), h);
    if (fd < 0) {
        vfs_file_unref(h);
        return fd;
    }
    if (flags & IN_CLOEXEC)
        proc_current()->fd_cloexec |= (1ULL << fd);
    return fd;
}

int64_t sys_inotify_init(void)
{
    return sys_inotify_init1(0);
}

int64_t sys_inotify_add_watch(int fd, uint64_t upath, uint32_t mask)
{
    inotify_inst_t *inst;
    int e = inst_of(fd, &inst);
    if (e < 0)
        return e;
    if (mask & ~(uint32_t)IN_WATCHABLE_MASK)
        return -E_INVAL;

    const char *path = (const char *)(uintptr_t)upath;
    if (!path || path[0] != '/' || strlen(path) >= GNUOS_PATH_MAX)
        return -E_INVAL;

    /* Re-adding an existing path updates its mask (IN_MASK_ADD ORs it in,
     * plain add replaces), which is how callers raise their interest.
     * The watch array is shared with the fire path, so the scan and the
     * commit sit inside the same critical section. */
    mutex_lock(&g_inotify_mutex);
    inotify_watch_t *slot = NULL;
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (inst->watches[i].used &&
            strcmp(inst->watches[i].path, path) == 0) {
            if (mask & IN_MASK_CREATE) {
                mutex_unlock(&g_inotify_mutex);
                return -E_EXIST;
            }
            if (mask & IN_MASK_ADD)
                inst->watches[i].mask |= mask & ~IN_MASK_ADD;
            else
                inst->watches[i].mask = mask & ~IN_MASK_ADD;
            int wd = inst->watches[i].wd;
            mutex_unlock(&g_inotify_mutex);
            return wd;
        }
        if (!slot && !inst->watches[i].used)
            slot = &inst->watches[i];
    }
    if (!slot) {
        mutex_unlock(&g_inotify_mutex);
        return -E_NOSPC;
    }

    slot->used = 1;
    slot->wd   = inst->next_wd++;
    slot->mask = mask & ~(uint32_t)(IN_MASK_ADD | IN_MASK_CREATE);
    strcpy(slot->path, path);
    int wd = slot->wd;
    mutex_unlock(&g_inotify_mutex);
    return wd;
}

int64_t sys_inotify_rm_watch(int fd, uint32_t wd)
{
    inotify_inst_t *inst;
    int e = inst_of(fd, &inst);
    if (e < 0)
        return e;
    mutex_lock(&g_inotify_mutex);
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (inst->watches[i].used && inst->watches[i].wd == (int)wd) {
            inst->watches[i].used = 0;
            /* IN_IGNORED tells the watcher the wd is dead. */
            inst_enqueue(inst, (int)wd, IN_IGNORED, 0, NULL);
            mutex_unlock(&g_inotify_mutex);
            return 0;
        }
    }
    mutex_unlock(&g_inotify_mutex);
    return -E_INVAL;
}
