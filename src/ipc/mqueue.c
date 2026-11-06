/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mqueue.c — POSIX message queues. (GPLv2)
 *
 * See mqueue.h for the ABI contract.  The module follows the sysvipc
 * house style: one static pool, all syscalls serialised by the big kernel
 * lock, blocking send/receive parked on per-queue wait queues (the PCB's
 * wait_q names the queue; sched_wake_queue wakes every sleeper on it, and
 * a sleeper hands the lock to whoever wakes it, so every check-then-act
 * below is atomic against other processes), and an anonfd per open() so
 * the queue descriptor is a real poll(2)able file descriptor like Linux's.
 *
 * The time base for the *_timed* calls is CLOCK_REALTIME, exactly what
 * musl passes: the same epoch + tick arithmetic sys_clock_gettime uses,
 * so a caller that mixes clock_gettime(REALTIME) with mq_timedreceive
 * deadlines gets consistent results.
 */
#include <stdint.h>

#include "mqueue.h"
#include "proc.h"
#include "vfs.h"
#include "vmm.h"
#include "heap.h"
#include "kstring.h"
#include "timer.h"
#include "syscall.h"
#include "anonfd.h" /* anon_bind */

/* ---- pool --------------------------------------------------------------- */

#define MQ_MAX         16   /* simultaneous queues */
#define MQ_MAXMSG_MAX  128  /* per-queue message capacity cap */
#define MQ_MSGSIZE_MAX 8192 /* per-message size cap */
#define MQ_NAME_MAX    256
#define MQ_PRIO_MAX    32768

typedef struct mq {
    int      used;
    int      unlinked; /* unlinked: no new open(), die on last close */
    int      refs;     /* open file descriptors onto this queue */
    char     name[MQ_NAME_MAX];
    long     maxmsg;
    long     msgsize;
    long     curmsgs;
    long     head;      /* ring index of the oldest message */
    int      nonblock; /* the queue-level O_NONBLOCK (mq_getsetattr) */
    uint8_t *msgs;     /* maxmsg * msgsize ring of message slots */
    unsigned *lens;    /* per-slot length */
    unsigned *prios;   /* per-slot priority */

    /* Wake channels: receivers parked when empty, senders when full. */
    proc_t  *rwait;
    proc_t  *wwait;

    /* mq_notify registration (one per queue, like Linux) and the pid it
     * signals (SIGEV_SIGNAL is delivered to the registering process). */
    int         has_notify;
    mq_notify_t notify;
    int         notify_pid;
} mq_t;

static mq_t g_mqs[MQ_MAX];

static int  mq_poll(vfs_node_t *n, int16_t events, int16_t *revents);
static void mq_release(vfs_node_t *n);
static const vfs_ops_t g_mq_ops;

/* ---- helpers ------------------------------------------------------------ */

static int mq_name_copy(uint64_t uname, char out[MQ_NAME_MAX])
{
    if (!user_ptr_ok(uname, 1))
        return -1;
    const char *s = (const char *)(uintptr_t)uname;
    uint32_t    i = 0;
    for (; i < MQ_NAME_MAX; i++) {
        if (!user_ptr_ok(uname + i, 1))
            return -1;
        out[i] = s[i];
        if (!out[i])
            break;
    }
    if (i == MQ_NAME_MAX)
        return -1; /* unterminated */
    if (out[0] != '/')
        return -1; /* POSIX mqueue names start with '/' */
    return 0;
}

/* fd -> live queue, for the syscalls that take a descriptor. */
static mq_t *mq_by_fd(int64_t mqd)
{
    if (mqd < 0 || mqd >= PROC_MAX_FD)
        return NULL;
    int h = fd_handle((int)mqd);
    if (h < 0)
        return NULL;
    vfs_node_t *n = vfs_file_node(h);
    if (!n || n->kind != VFS_ANON || n->ops != &g_mq_ops)
        return NULL;
    mq_t *q = (mq_t *)n->priv;
    return (q && q->used && !q->unlinked) ? q : NULL;
}

static int64_t mq_now_realtime(void)
{
    return (int64_t)(timer_ticks() / SCHED_HZ) + (int64_t)timer_boot_epoch();
}

/* Absolute CLOCK_REALTIME timespec (user pointer) -> relative ticks until
 * the deadline; (uint64_t)-1 = no timeout given, 0 = already expired. */
static uint64_t mq_deadline_ticks(uint64_t timeout)
{
    if (!timeout)
        return (uint64_t)-1;
    if (!user_ptr_ok(timeout, 16))
        return 0;
    int64_t sec  = *(int64_t *)(uintptr_t)timeout;
    int64_t nsec = *(int64_t *)(uintptr_t)(timeout + 8);

    int64_t rel = sec - mq_now_realtime();
    if (rel < 0)
        return 0;
    uint64_t ticks = (uint64_t)rel * SCHED_HZ;
    ticks += (uint64_t)nsec / (1000000000ULL / SCHED_HZ);
    return ticks ? ticks : 1;
}

/* Park on the queue's wake channel until sched_wake_queue() (or the
 * deadline runs out); the caller must re-check its condition and loop. */
static void mq_wait(proc_t **wq, uint64_t ticks)
{
    proc_t *me = proc_current();
    if (!me)
        return;
    me->wait_q = wq;
    if (ticks != (uint64_t)-1)
        me->wake_tick = timer_ticks() + ticks;
    sched_block(WAIT_IPC);
    me->wait_q    = NULL;
    me->wake_tick = 0;
}

static void mq_wake(mq_t *q, int readers)
{
    if (readers) {
        if (q->rwait)
            sched_wake_queue(&q->rwait);
    } else {
        if (q->wwait)
            sched_wake_queue(&q->wwait);
    }
    /* poll(2) sleepers park on the generic channel (eventfd's pattern). */
    sched_wake_reason(WAIT_PIPE);
}

/* Deliver the registered notification: only when the queue transitions
 * empty -> non-empty, and the registration is consumed (one-shot), like
 * Linux. */
static void mq_maybe_notify(mq_t *q)
{
    if (!q->has_notify)
        return;
    mq_notify_t n = q->notify;
    q->has_notify = 0;

    if (n.notify == SIGEV_SIGNAL || n.notify == SIGEV_THREAD_ID) {
        /* GNOS has no separate tid space: SIGEV_THREAD_ID's tid is the
         * registering pid here, same as SIGEV_SIGNAL's implicit target. */
        proc_t *p = proc_by_pid(q->notify_pid);
        if (p)
            proc_signal(p, n.signo);
    }
    /* SIGEV_NONE registers an empty notification: waking a blocked
     * receiver is all the caller asked for. */
}

/* ---- fd side (poll / release) ------------------------------------------- */

static int mq_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    mq_t *q = (mq_t *)n->priv;
    int16_t r = 0;
    if (q && q->used && !q->unlinked) {
        if (events & POLLIN)
            r |= (q->curmsgs > 0) ? POLLIN : 0;
        if (events & POLLOUT)
            r |= (q->curmsgs < q->maxmsg) ? POLLOUT : 0;
    }
    *revents = r;
    return 0;
}

static void mq_release(vfs_node_t *n)
{
    mq_t *q = (mq_t *)n->priv;
    n->priv = NULL;
    if (!q)
        return;
    q->refs--;
    if (q->refs <= 0 && q->unlinked) {
        /* Last descriptor onto an unlinked queue: retire it.  Sleepers
         * were woken by sys_mq_unlink; free the storage now. */
        if (q->msgs)
            kfree(q->msgs);
        if (q->lens)
            kfree(q->lens);
        if (q->prios)
            kfree(q->prios);
        q->used = 0;
    }
}

static const vfs_ops_t g_mq_ops = {
    .poll    = mq_poll,
    .release = mq_release,
};

/* ---- syscalls ------------------------------------------------------------ */

int64_t sys_mq_open(uint64_t uname, uint64_t oflag, uint64_t mode, uint64_t uattr)
{
    (void)mode;
    char name[MQ_NAME_MAX];
    if (mq_name_copy(uname, name) < 0)
        return -E_INVAL;

    mq_t *q = NULL;
    for (int i = 0; i < MQ_MAX; i++) {
        if (g_mqs[i].used && !g_mqs[i].unlinked &&
            strcmp(g_mqs[i].name, name) == 0) {
            q = &g_mqs[i];
            break;
        }
    }

    if (q && (oflag & MQ_O_EXCL))
        return -E_EXIST;

    if (!q) {
        if (!(oflag & MQ_O_CREAT))
            return -E_NOENT;
        mq_attr_t a;
        if (!uattr || !user_ptr_ok(uattr, sizeof a))
            return -E_INVAL;
        memcpy(&a, (void *)(uintptr_t)uattr, sizeof a);
        if (a.mq_maxmsg <= 0 || a.mq_msgsize <= 0 ||
            a.mq_maxmsg > MQ_MAXMSG_MAX || a.mq_msgsize > MQ_MSGSIZE_MAX)
            return -E_INVAL;

        for (int i = 0; i < MQ_MAX; i++) {
            if (!g_mqs[i].used) {
                q = &g_mqs[i];
                break;
            }
        }
        if (!q)
            return -E_MFILE;

        memset(q, 0, sizeof *q);
        q->maxmsg  = a.mq_maxmsg;
        q->msgsize = a.mq_msgsize;
        strncpy(q->name, name, sizeof(q->name) - 1);
        q->msgs  = kmalloc((uint32_t)q->maxmsg * (uint32_t)q->msgsize);
        q->lens  = kmalloc((uint32_t)q->maxmsg * sizeof(unsigned));
        q->prios = kmalloc((uint32_t)q->maxmsg * sizeof(unsigned));
        if (!q->msgs || !q->lens || !q->prios) {
            if (q->msgs) kfree(q->msgs);
            if (q->lens) kfree(q->lens);
            if (q->prios) kfree(q->prios);
            return -E_NOMEM;
        }
        q->used = 1;
    }

    if (q->unlinked) /* raced with an unlink between scan and bind */
        return -E_NOENT;
    q->refs++;

    /* Bind the fd: anon node with the queue as priv.  musl's mq_send calls
     * mq_timedsend(fd, ...), and poll() on the fd works like on Linux. */
    int h = vfs_anon_open(VFS_ANON, &g_mq_ops, q,
                          (int)(oflag & 3) | ((oflag & O_NONBLOCK) ? O_NONBLOCK : 0));
    if (h < 0) {
        q->refs--;
        return h;
    }
    int fd = anon_bind(h, (oflag & O_CLOEXEC) != 0);
    if (fd < 0) {
        vfs_file_unref(h);
        q->refs--;
        return fd;
    }
    return fd;
}

int64_t sys_mq_unlink(uint64_t uname)
{
    char name[MQ_NAME_MAX];
    if (mq_name_copy(uname, name) < 0)
        return -E_INVAL;

    for (int i = 0; i < MQ_MAX; i++) {
        mq_t *q = &g_mqs[i];
        if (q->used && !q->unlinked && strcmp(q->name, name) == 0) {
            q->unlinked = 1;
            /* Every sleeper learns the queue is gone; blocked receivers
             * and senders re-check and see unlinked -> EIDRM-shaped
             * errors (Linux returns EBADF for fds onto a removed queue's
             * dead object; we mirror that in the send/receive loops). */
            q->rwait = NULL;
            q->wwait = NULL;
            mq_wake(q, 1);
            mq_wake(q, 0);
            if (q->refs <= 0) {
                if (q->msgs) kfree(q->msgs);
                if (q->lens) kfree(q->lens);
                if (q->prios) kfree(q->prios);
                q->used = 0;
            }
            return 0;
        }
    }
    return -E_NOENT;
}

int64_t sys_mq_timedsend(uint64_t mqd, uint64_t umsg, uint64_t len,
                         uint64_t prio, uint64_t timeout)
{
    mq_t *q = mq_by_fd((int64_t)(int)mqd);
    if (!q)
        return -E_BADF;
    if (len > (uint64_t)q->msgsize)
        return -E_MSGSIZE;
    if (prio >= MQ_PRIO_MAX)
        return -E_INVAL;
    if (len && !user_ptr_ok(umsg, (uint32_t)len))
        return -E_FAULT;

    uint64_t deadline = mq_deadline_ticks(timeout);
    uint64_t spin_deadline = 0;
    if (deadline != (uint64_t)-1)
        spin_deadline = timer_ticks() + deadline;

    int fds_nonblock = 0;
    /* The descriptor's O_NONBLOCK (set at open) and the queue-level flag
     * (mq_setattr) both make send/receive nonblocking, like Linux. */
    {
        int h = fd_handle((int)mqd);
        if (h >= 0)
            fds_nonblock = (vfs_file_flags(h) & O_NONBLOCK) != 0;
    }

    for (;;) {
        if (q->unlinked)
            return -E_BADF;
        if (q->curmsgs < q->maxmsg) {
            /* Enqueue at priority order: highest prio at the head.  A
             * linear insert is fine at maxmsg <= 128. */
            long pos = 0;
            while (pos < q->curmsgs && q->prios[(q->head + pos) % q->maxmsg] >= prio)
                pos++;
            /* shift the tail portion up by one slot */
            for (long i = q->curmsgs; i > pos; i--) {
                long dst = (q->head + i) % q->maxmsg;
                long src = (q->head + i - 1) % q->maxmsg;
                memcpy(q->msgs + (uint32_t)dst * (uint32_t)q->msgsize,
                       q->msgs + (uint32_t)src * (uint32_t)q->msgsize,
                       (uint32_t)q->msgsize);
                q->lens[(q->head + i) % q->maxmsg]  = q->lens[(q->head + i - 1) % q->maxmsg];
                q->prios[(q->head + i) % q->maxmsg] = q->prios[(q->head + i - 1) % q->maxmsg];
            }
            long slot = (q->head + pos) % q->maxmsg;
            if (len)
                memcpy(q->msgs + (uint32_t)slot * (uint32_t)q->msgsize,
                       (void *)(uintptr_t)umsg, (uint32_t)len);
            q->lens[slot]  = (unsigned)len;
            q->prios[slot] = (unsigned)prio;
            q->curmsgs++;

            int was_empty = (q->curmsgs == 1);
            mq_wake(q, 1); /* a parked receiver may take it now */
            if (was_empty)
                mq_maybe_notify(q);
            return 0;
        }

        if (fds_nonblock || q->nonblock)
            return -E_AGAIN;

        /* Full: park.  A wake without a free slot loops; a deadline that
         * has passed returns ETIMEDOUT, matching Linux. */
        uint64_t ticks = (uint64_t)-1;
        if (spin_deadline) {
            if (timer_ticks() >= spin_deadline)
                return -E_TIMEDOUT;
            ticks = spin_deadline - timer_ticks();
        }
        mq_wait(&q->wwait, ticks);
    }
}

int64_t sys_mq_timedreceive(uint64_t mqd, uint64_t umsg, uint64_t len,
                            uint64_t uprio, uint64_t timeout)
{
    mq_t *q = mq_by_fd((int64_t)(int)mqd);
    if (!q)
        return -E_BADF;
    if (len < (uint64_t)q->msgsize)
        return -E_MSGSIZE; /* the caller's buffer must hold any message */
    if (len && !user_ptr_ok(umsg, (uint32_t)len))
        return -E_FAULT;
    if (uprio && !user_ptr_ok(uprio, sizeof(unsigned)))
        return -E_FAULT;

    uint64_t deadline = mq_deadline_ticks(timeout);
    uint64_t spin_deadline = 0;
    if (deadline != (uint64_t)-1)
        spin_deadline = timer_ticks() + deadline;

    int fds_nonblock = 0;
    {
        int h = fd_handle((int)mqd);
        if (h >= 0)
            fds_nonblock = (vfs_file_flags(h) & O_NONBLOCK) != 0;
    }

    for (;;) {
        if (q->unlinked)
            return -E_BADF;
        if (q->curmsgs > 0) {
            long slot = q->head;
            unsigned mlen = q->lens[slot];
            if (mlen)
                memcpy((void *)(uintptr_t)umsg,
                       q->msgs + (uint32_t)slot * (uint32_t)q->msgsize, mlen);
            if (uprio)
                *(unsigned *)(uintptr_t)uprio = q->prios[slot];
            q->head = (q->head + 1) % q->maxmsg;
            q->curmsgs--;

            int was_full = (q->curmsgs == q->maxmsg - 1);
            mq_wake(q, 0); /* a parked sender may have room now */
            if (was_full)
                mq_maybe_notify(q);
            return (int64_t)mlen;
        }

        if (fds_nonblock || q->nonblock)
            return -E_AGAIN;

        uint64_t ticks = (uint64_t)-1;
        if (spin_deadline) {
            if (timer_ticks() >= spin_deadline)
                return -E_TIMEDOUT;
            ticks = spin_deadline - timer_ticks();
        }
        mq_wait(&q->rwait, ticks);
    }
}

int64_t sys_mq_notify(uint64_t mqd, uint64_t usevp)
{
    mq_t *q = mq_by_fd((int64_t)(int)mqd);
    if (!q)
        return -E_BADF;

    if (!usevp) {
        /* notify(NULL) deregisters, but only the owner may. */
        q->has_notify = 0;
        return 0;
    }
    if (!user_ptr_ok(usevp, 32))
        return -E_FAULT;

    proc_t *me = proc_current();
    if (!me)
        return -E_PERM;
    if (q->has_notify && q->notify_pid != me->pid)
        return -E_BUSY; /* another process holds the notification */

    /* musl's struct sigevent (x86-64, 32 bytes):
     * int sigev_notify; int sigev_signo; union sigval value; then the
     * SIGEV_THREAD fields, which the kernel never touches. */
    int  notify = *(int *)(uintptr_t)usevp;
    int  signo  = *(int *)(uintptr_t)(usevp + 4);
    uint64_t value = *(uint64_t *)(uintptr_t)(usevp + 8);
    if (notify == SIGEV_THREAD)
        return -E_INVAL; /* musl implements THREAD in user space */
    if (notify != SIGEV_NONE && notify != SIGEV_SIGNAL &&
        notify != SIGEV_THREAD_ID)
        return -E_INVAL;
    if (notify == SIGEV_SIGNAL || notify == SIGEV_THREAD_ID) {
        if (signo <= 0 || signo >= 64)
            return -E_INVAL;
    }

    q->notify.notify = notify;
    q->notify.signo  = signo;
    q->notify.value  = value;
    q->notify.tid    = me->pid;
    q->notify_pid    = me->pid;
    q->has_notify    = 1;

    /* POSIX: registering while messages are pending delivers at once. */
    if (q->curmsgs > 0)
        mq_maybe_notify(q);
    return 0;
}

int64_t sys_mq_getsetattr(uint64_t mqd, uint64_t unew, uint64_t uold)
{
    mq_t *q = mq_by_fd((int64_t)(int)mqd);
    if (!q)
        return -E_BADF;

    if (unew) {
        mq_attr_t a;
        if (!user_ptr_ok(unew, sizeof a))
            return -E_FAULT;
        memcpy(&a, (void *)(uintptr_t)unew, sizeof a);
        /* Only O_NONBLOCK is settable after open, like Linux. */
        q->nonblock = (a.mq_flags & O_NONBLOCK) != 0;
    }

    if (uold) {
        mq_attr_t a;
        a.mq_flags   = q->nonblock ? O_NONBLOCK : 0;
        a.mq_maxmsg  = q->maxmsg;
        a.mq_msgsize = q->msgsize;
        a.mq_curmsgs = q->curmsgs;
        if (!user_ptr_ok(uold, sizeof a))
            return -E_FAULT;
        memcpy((void *)(uintptr_t)uold, &a, sizeof a);
    }
    return 0;
}
