/*
 * sysvipc.c — System V IPC: message queues, semaphores, shared memory.
 * (GPLv2)
 *
 * See sysvipc.h for the ABI contract.  Everything here runs under the big
 * kernel lock (user syscalls are serialised by it), and a task that blocks
 * hands the lock to whoever wakes it, so check-then-act sequences between
 * "is there a message / is the semaphore available" and "sleep" are atomic
 * against every other process.  The one cross-context wake path is the
 * timer (semtimedop deadlines), which only ever wakes a sleeper to re-check
 * its condition.
 *
 * Object tables are static pools with monotonic identifiers: an id encodes
 * (generation << 8 | slot), so a stale id from a removed object can never
 * collide with a later object that reuses the slot.
 */
#include <stdint.h>

#include "sysvipc.h"
#include "proc.h"
#include "vfs.h"
#include "vmm.h"
#include "heap.h"
#include "kstring.h"
#include "timer.h"
#include "pmm.h"

/* ------------------------------------------------------------------ */
/* common                                                             */
/* ------------------------------------------------------------------ */

/* Kernel seconds since boot; ipc_perm ctime fields are wall-time on Linux
 * but there is no RTC clock here, so a monotonic second counter keeps the
 * fields non-zero, ordered, and honest about their granularity. */
static int64_t ipc_time(void)
{
    return (int64_t)(timer_ticks() / SCHED_HZ);
}

/* Sleep until somebody sched_wake_queue()s `q` (or the timeout ticks run
 * out, when `ticks` is non-zero).  Returns after the wake; the caller must
 * re-check its condition and loop. */
static void ipc_wait(void *q, uint64_t ticks)
{
    proc_t *me = proc_current();
    if (!me)
        return;
    me->wait_q = q;
    if (ticks) {
        me->wake_tick = timer_ticks() + ticks;
        sched_block(WAIT_IPC);
    } else {
        sched_block(WAIT_IPC);
    }
    me->wait_q = NULL;
    me->wake_tick = 0;
}

static void ipc_wake_all(void *q)
{
    sched_wake_queue(q);
}

/* Classic SysV permission check against mode bits (0400 read, 0200 write,
 * shifted down for group/other), with uid 0 bypassing everything. */
static int ipc_permitted(const kipc_perm_t *perm, int want_read_write)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_ACCES;
    if (p->euid == 0)
        return 0;
    int bit = (want_read_write == 1) ? 04 : 02;
    int owner = (p->euid == perm->uid || p->euid == perm->cuid);
    int in_grp = (p->egid == perm->gid || p->egid == perm->cgid);
    uint32_t eff = owner ? (perm->mode >> 6)
                 : in_grp ? (perm->mode >> 3)
                          : perm->mode;
    return (eff & (uint32_t)bit) ? 0 : -E_ACCES;
}

/* Encode/decode object identifiers. */
#define ID_SHIFT 8
static int32_t make_id(uint16_t seq, int slot)
{
    return (int32_t)(((uint32_t)seq << ID_SHIFT) | (uint32_t)slot);
}

/* ------------------------------------------------------------------ */
/* message queues                                                     */
/* ------------------------------------------------------------------ */

#define MSGQ_MAX   128
#define MSG_MAX_SZ 8192         /* MSGMAX: largest single message  */
#define MSGQ_DEF_BYTES 16384    /* MSGMNB: default bytes per queue */

typedef struct ipc_msg {
    struct ipc_msg *next;
    long            mtype;
    uint32_t        msize;      /* payload bytes */
    char            data[];     /* msize bytes, heap-allocated */
} ipc_msg_t;

typedef struct {
    int         used;
    uint16_t    seq;
    int32_t     key;
    kipc_perm_t perm;
    ipc_msg_t  *head, *tail;
    uint64_t    qnum, cbytes, qbytes;
    int64_t     stime, rtime, ctime;
    int32_t     lspid, lrpid;
    char        wake;           /* dummy wait_q target: &q->wake */
} msgq_t;

static msgq_t g_msgq[MSGQ_MAX];

static msgq_t *msgq_at(int msqid)
{
    int slot = msqid & ((1 << ID_SHIFT) - 1);
    if (slot < 0 || slot >= MSGQ_MAX || !g_msgq[slot].used)
        return NULL;
    if ((uint32_t)(msqid >> ID_SHIFT) != g_msgq[slot].seq)
        return NULL;
    return &g_msgq[slot];
}

static int msgq_alloc_slot(void)
{
    static uint16_t s_seq;
    for (int i = 0; i < MSGQ_MAX; i++) {
        if (g_msgq[i].used)
            continue;
        g_msgq[i].used = 1;
        g_msgq[i].seq  = ++s_seq;
        return i;
    }
    return -1;
}

static void msgq_free_msgs(msgq_t *q)
{
    ipc_msg_t *m = q->head;
    while (m) {
        ipc_msg_t *n = m->next;
        kfree(m);
        m = n;
    }
    q->head = q->tail = NULL;
    q->qnum = q->cbytes = 0;
}

long sysv_msgget(int32_t key, int flags)
{
    /* An existing queue wins unless IPC_CREAT|IPC_EXCL demands a fresh one.
     * A private key (0) never matches: every IPC_PRIVATE get creates. */
    for (int i = 0; i < MSGQ_MAX; i++) {
        if (!g_msgq[i].used || key == IPC_PRIVATE ||
            g_msgq[i].key != key)
            continue;
        if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL))
            return -E_EXIST;
        int r = ipc_permitted(&g_msgq[i].perm, 1);
        if (r < 0)
            return r;
        return make_id(g_msgq[i].seq, i);
    }
    if (!(flags & IPC_CREAT))
        return -E_NOENT;

    int slot = msgq_alloc_slot();
    if (slot < 0)
        return -E_NFILE;                /* MSGMNI exhausted */
    msgq_t *q = &g_msgq[slot];
    proc_t *p = proc_current();
    memset(q, 0, sizeof(*q));
    q->key    = key;
    q->perm.key  = key;
    q->perm.mode = (uint32_t)(flags & 0777);
    q->perm.uid = q->perm.cuid = p ? p->euid : 0;
    q->perm.gid = q->perm.cgid = p ? p->egid : 0;
    q->qbytes  = MSGQ_DEF_BYTES;
    q->ctime   = ipc_time();
    return make_id(q->seq, slot);
}

/* Find the first message matching msgtyp (Linux selection rules).  Returns
 * the node, or NULL. */
static ipc_msg_t *msgq_find(msgq_t *q, long msgtyp)
{
    for (ipc_msg_t *m = q->head; m; m = m->next) {
        if (msgtyp == 0)
            return m;                       /* any */
        if (msgtyp > 0) {
            if (m->mtype == msgtyp)
                return m;                   /* exact type */
        } else {
            /* msgtyp < 0: the lowest positive type <= -msgtyp */
            if (m->mtype <= -msgtyp)
                return m;
        }
    }
    return NULL;
}

static void msgq_remove(msgq_t *q, ipc_msg_t *m)
{
    ipc_msg_t **pp = &q->head;
    while (*pp && *pp != m)
        pp = &(*pp)->next;
    if (*pp == m) {
        *pp = m->next;
        if (q->tail == m)
            q->tail = NULL;
        if (!q->head)
            q->tail = NULL;
    }
    q->qnum--;
    q->cbytes -= m->msize;
    kfree(m);
}

long sysv_msgsnd(int msqid, uint64_t umsgp, size_t msgsz, int flags)
{
    if (msgsz > MSG_MAX_SZ)
        return -E_INVAL;
    if (!user_ptr_ok(umsgp, msgsz + sizeof(long)))
        return -E_FAULT;

    long mtype;
    memcpy(&mtype, (const void *)(uintptr_t)umsgp, sizeof(long));
    if (mtype <= 0)
        return -E_INVAL;

    msgq_t *q = msgq_at(msqid);
    if (!q)
        return -E_INVAL;
    int r = ipc_permitted(&q->perm, 2);
    if (r < 0)
        return r;
    if (msgsz > q->qbytes)
        return -E_INVAL;

    proc_t *p = proc_current();
    for (;;) {
        if (!q->used)
            return -E_IDRM;
        if (q->cbytes + msgsz > q->qbytes) {
            if (flags & IPC_NOWAIT)
                return -E_AGAIN;
            ipc_wait(&q->wake, 0);
            continue;
        }
        break;
    }

    ipc_msg_t *m = kmalloc(sizeof(*m) + msgsz);
    if (!m)
        return -E_NOMEM;
    m->mtype = mtype;
    m->msize = (uint32_t)msgsz;
    m->next  = NULL;
    if (msgsz)
        memcpy(m->data, (const void *)(uintptr_t)(umsgp + sizeof(long)), msgsz);

    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
    q->qnum++;
    q->cbytes += msgsz;
    q->stime  = ipc_time();
    q->lspid  = p ? p->pid : 0;
    ipc_wake_all(&q->wake);
    return 0;
}

long sysv_msgrcv(int msqid, uint64_t umsgp, size_t msgsz, long msgtyp,
                 int flags)
{
    if (!user_ptr_ok(umsgp, msgsz + sizeof(long)))
        return -E_FAULT;

    msgq_t *q = msgq_at(msqid);
    if (!q)
        return -E_INVAL;
    int r = ipc_permitted(&q->perm, 1);
    if (r < 0)
        return r;

    proc_t *p = proc_current();
    for (;;) {
        if (!q->used)
            return -E_IDRM;
        ipc_msg_t *m = msgq_find(q, msgtyp);
        if (m) {
            uint32_t take = m->msize;
            if (take > msgsz) {
                if (!(flags & MSG_NOERROR))
                    return -E_2BIG;
                take = (uint32_t)msgsz;         /* truncated delivery */
            }
            /* Copy the payload out of the node, then free it: after
             * msgq_remove() the node is gone. */
            long mt = m->mtype;
            uint64_t dst = umsgp + sizeof(long);
            if (take)
                memcpy((void *)(uintptr_t)dst, m->data, take);
            msgq_remove(q, m);
            q->rtime = ipc_time();
            q->lrpid = p ? p->pid : 0;
            memcpy((void *)(uintptr_t)umsgp, &mt, sizeof(long));
            return (long)take;
        }
        if (flags & IPC_NOWAIT)
            return -E_MSG;              /* ENOMSG */
        ipc_wait(&q->wake, 0);
    }
}

long sysv_msgctl(int msqid, int cmd, uint64_t ubuf)
{
    msgq_t *q = msgq_at(msqid);
    if (!q)
        return -E_INVAL;

    switch (cmd) {
    case IPC_RMID:
        q->used = 0;
        msgq_free_msgs(q);
        ipc_wake_all(&q->wake);         /* sleepers re-check and see EIDRM */
        return 0;
    case IPC_STAT: {
        if (!user_ptr_ok(ubuf, sizeof(kmsqid_ds_t)))
            return -E_FAULT;
        int r = ipc_permitted(&q->perm, 1);
        if (r < 0)
            return r;
        kmsqid_ds_t ds;
        memset(&ds, 0, sizeof(ds));
        memcpy(&ds.perm, &q->perm, sizeof(ds.perm));
        ds.stime  = q->stime;
        ds.rtime  = q->rtime;
        ds.ctime  = q->ctime;
        ds.cbytes = q->cbytes;
        ds.qnum   = q->qnum;
        ds.qbytes = q->qbytes;
        ds.lspid  = q->lspid;
        ds.lrpid  = q->lrpid;
        memcpy((void *)(uintptr_t)ubuf, &ds, sizeof(ds));
        return 0;
    }
    case IPC_SET: {
        if (!user_ptr_ok(ubuf, sizeof(kmsqid_ds_t)))
            return -E_FAULT;
        proc_t *p = proc_current();
        if (p && p->euid != 0 &&
            p->euid != q->perm.uid && p->euid != q->perm.cuid)
            return -E_PERM;
        kmsqid_ds_t ds;
        memcpy(&ds, (const void *)(uintptr_t)ubuf, sizeof(ds));
        q->perm.uid  = ds.perm.uid;
        q->perm.gid  = ds.perm.gid;
        q->perm.mode = ds.perm.mode & 0777u;
        return 0;
    }
    default:
        return -E_INVAL;                /* IPC_INFO/MSG_STAT not exposed */
    }
}

/* ------------------------------------------------------------------ */
/* semaphores                                                          */
/* ------------------------------------------------------------------ */

#define SEMSET_MAX 128
#define SEM_NSEMS_MAX 128

typedef struct ksem {
    int32_t val;
    int32_t pid;
    int32_t ncnt;               /* waiters for val to rise */
    int32_t zcnt;               /* waiters for val to hit 0 */
} ksem_t;

/* SEM_UNDO adjustment record, one per (process, set) pair. */
typedef struct semundo {
    struct semundo *next;
    int32_t         pid;
    int32_t        *adj;        /* nsems entries; undone on process exit */
} semundo_t;

typedef struct {
    int         used;
    uint16_t    seq;
    int32_t     key;
    kipc_perm_t perm;
    uint16_t    nsems;
    ksem_t     *sems;           /* heap array of nsems */
    semundo_t  *undos;
    int64_t     otime, ctime;
    char        wake;
} ksem_set_t;

static ksem_set_t g_semset[SEMSET_MAX];

static ksem_set_t *semset_at(int semid)
{
    int slot = semid & ((1 << ID_SHIFT) - 1);
    if (slot < 0 || slot >= SEMSET_MAX || !g_semset[slot].used)
        return NULL;
    if ((uint32_t)(semid >> ID_SHIFT) != g_semset[slot].seq)
        return NULL;
    return &g_semset[slot];
}

static int semset_alloc_slot(void)
{
    static uint16_t s_seq;
    for (int i = 0; i < SEMSET_MAX; i++) {
        if (g_semset[i].used)
            continue;
        g_semset[i].used = 1;
        g_semset[i].seq  = ++s_seq;
        return i;
    }
    return -1;
}

long sysv_semget(int32_t key, int nsems, int flags)
{
    for (int i = 0; i < SEMSET_MAX; i++) {
        if (!g_semset[i].used || key == IPC_PRIVATE ||
            g_semset[i].key != key)
            continue;
        if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL))
            return -E_EXIST;
        int r = ipc_permitted(&g_semset[i].perm, 1);
        if (r < 0)
            return r;
        if (nsems > g_semset[i].nsems)
            return -E_INVAL;
        return make_id(g_semset[i].seq, i);
    }
    if (!(flags & IPC_CREAT))
        return -E_NOENT;
    if (nsems <= 0 || nsems > SEM_NSEMS_MAX)
        return -E_INVAL;

    int slot = semset_alloc_slot();
    if (slot < 0)
        return -E_NFILE;
    ksem_set_t *st = &g_semset[slot];
    proc_t *p = proc_current();
    memset(st, 0, sizeof(*st));
    st->key    = key;
    st->perm.key  = key;
    st->perm.mode = (uint32_t)(flags & 0777);
    st->perm.uid = st->perm.cuid = p ? p->euid : 0;
    st->perm.gid = st->perm.cgid = p ? p->egid : 0;
    st->nsems   = (uint16_t)nsems;
    st->ctime   = ipc_time();
    st->sems    = kmalloc(sizeof(ksem_t) * (size_t)nsems);
    if (!st->sems) {
        st->used = 0;
        return -E_NOMEM;
    }
    memset(st->sems, 0, sizeof(ksem_t) * (size_t)nsems);
    return make_id(st->seq, slot);
}

/* One sembuf operation, kernel layout == musl's (ushort,short,short). */
typedef struct {
    uint16_t num;
    int16_t  op;
    int16_t  flg;
} kops_t;

/* Would applying the whole sops array leave every value >= 0?  If yes,
 * apply it, account SEM_UNDO and wake the set's waiters.  Returns 1 when
 * applied, 0 when it must wait.  `counted` marks which semaphores already
 * incremented their waiter count on a previous failed pass. */
static int semop_try(ksem_set_t *st, const kops_t *sops, size_t nsops,
                     uint32_t *counted)
{
    int16_t  sem_ok = 1;
    int      inc_ncnt[128];             /* up to SEM_NSEMS_MAX */

    for (size_t i = 0; i < nsops && i < 128; i++) {
        unsigned n = sops[i].num;
        if (n >= st->nsems)
            return -E_RANGE;
    }

    for (size_t i = 0; i < nsops; i++) {
        ksem_t *s = &st->sems[sops[i].num];
        int32_t v = s->val + sops[i].op;
        if (v < 0) {
            sem_ok = 0;
            if (!(sops[i].flg & IPC_NOWAIT)) {
                if (!(counted[sops[i].num] & 1)) {
                    if (sops[i].op < 0)
                        s->ncnt++;
                    else if (sops[i].op == 0)
                        s->zcnt++;
                    counted[sops[i].num] |= 1;
                }
            } else {
                return -E_AGAIN;
            }
        }
    }
    if (!sem_ok)
        return 0;                       /* sleep and retry */

    /* Everything fits: apply. */
    proc_t *p = proc_current();
    semundo_t *undo = NULL;
    for (size_t i = 0; i < nsops; i++) {
        ksem_t *s = &st->sems[sops[i].num];
        s->val += sops[i].op;
        s->pid  = p ? p->pid : 0;
        if (sops[i].flg & SEM_UNDO) {
            if (!undo) {
                for (semundo_t *u = st->undos; u; u = u->next)
                    if (u->pid == (p ? p->pid : 0)) {
                        undo = u;
                        break;
                    }
                if (!undo && p) {
                    undo = kmalloc(sizeof(*undo) +
                                   sizeof(int32_t) * st->nsems);
                    if (undo) {
                        undo->pid  = p->pid;
                        undo->adj  = (int32_t *)((char *)undo + sizeof(*undo));
                        memset(undo->adj, 0, sizeof(int32_t) * st->nsems);
                        undo->next = st->undos;
                        st->undos  = undo;
                    }
                }
            }
            if (undo)
                undo->adj[sops[i].num] -= sops[i].op;
        }
        if (counted[sops[i].num] & 1) {
            if (s->ncnt > 0)
                s->ncnt--;
            counted[sops[i].num] &= ~1u;
        }
    }
    st->otime = ipc_time();
    ipc_wake_all(&st->wake);
    return 1;
}

long sysv_semtimedop(int semid, uint64_t usops, size_t nsops,
                     uint64_t utimeout)
{
    if (nsops == 0 || nsops > 128)
        return -E_INVAL;
    if (!user_ptr_ok(usops, nsops * sizeof(kops_t)))
        return -E_FAULT;

    ksem_set_t *st = semset_at(semid);
    if (!st)
        return -E_INVAL;
    int r = ipc_permitted(&st->perm, 2);
    if (r < 0)
        return r;

    /* A relative timeout, like the sleep calls: convert sec/nsec to ticks.
     * (Linux's timeout is absolute; musl and our tests both use the
     * relative form in practice, and relative is what nanosleep-style
     * callers expect here.) */
    uint64_t ticks = 0;
    if (utimeout) {
        if (!user_ptr_ok(utimeout, 16))
            return -E_FAULT;
        int64_t sec, nsec;
        memcpy(&sec, (const void *)(uintptr_t)utimeout, 8);
        memcpy(&nsec, (const void *)(uintptr_t)(utimeout + 8), 8);
        if (sec < 0 || nsec < 0)
            return -E_INVAL;
        ticks = (uint64_t)sec * SCHED_HZ +
                (uint64_t)nsec / (1000000000ULL / SCHED_HZ);
    }

    kops_t sops[128];
    memcpy(sops, (const void *)(uintptr_t)usops, nsops * sizeof(kops_t));
    uint32_t counted[128];
    memset(counted, 0, sizeof(counted));

    uint64_t start = timer_ticks();
    for (;;) {
        if (!st->used)
            return -E_IDRM;
        r = semop_try(st, sops, nsops, counted);
        if (r != 0)
            return r < 0 ? r : 0;       /* applied (1) or hard error */
        if (ticks && timer_ticks() - start >= ticks)
            return -E_AGAIN;            /* timed out waiting */
        ipc_wait(&st->wake, ticks ? (ticks - (timer_ticks() - start)) : 0);
    }
}

long sysv_semop(int semid, uint64_t usops, size_t nsops)
{
    return sysv_semtimedop(semid, usops, nsops, 0);
}

/* The semun union is passed by pointer as the 4th syscall argument.  Only
 * the member the command needs is touched. */
long sysv_semctl(int semid, int semnum, int cmd, uint64_t uarg)
{
    ksem_set_t *st = semset_at(semid);
    if (!st)
        return -E_INVAL;

    switch (cmd) {
    case IPC_RMID: {
        int r = ipc_permitted(&st->perm, 2);
        if (r < 0)
            return r;
        /* Discard the set; undo records for it die with it, sleepers wake
         * to EIDRM on their next pass. */
        st->used = 0;
        semundo_t *u = st->undos;
        while (u) {
            semundo_t *n = u->next;
            kfree(u);
            u = n;
        }
        st->undos = NULL;
        kfree(st->sems);
        st->sems = NULL;
        ipc_wake_all(&st->wake);
        return 0;
    }
    case IPC_STAT: {
        if (!user_ptr_ok(uarg, sizeof(ksemid_ds_t)))
            return -E_FAULT;
        int r = ipc_permitted(&st->perm, 1);
        if (r < 0)
            return r;
        ksemid_ds_t ds;
        memset(&ds, 0, sizeof(ds));
        memcpy(&ds.perm, &st->perm, sizeof(ds.perm));
        ds.otime  = st->otime;
        ds.ctime  = st->ctime;
        ds.nsems  = st->nsems;
        memcpy((void *)(uintptr_t)uarg, &ds, sizeof(ds));
        return 0;
    }
    case IPC_SET: {
        if (!user_ptr_ok(uarg, sizeof(ksemid_ds_t)))
            return -E_FAULT;
        proc_t *p = proc_current();
        if (p && p->euid != 0 &&
            p->euid != st->perm.uid && p->euid != st->perm.cuid)
            return -E_PERM;
        ksemid_ds_t ds;
        memcpy(&ds, (const void *)(uintptr_t)uarg, sizeof(ds));
        st->perm.uid  = ds.perm.uid;
        st->perm.gid  = ds.perm.gid;
        st->perm.mode = ds.perm.mode & 0777u;
        return 0;
    }
    case GETPID:
        if (semnum < 0 || semnum >= st->nsems)
            return -E_RANGE;
        return st->sems[semnum].pid;
    case GETVAL:
        if (semnum < 0 || semnum >= st->nsems)
            return -E_RANGE;
        return st->sems[semnum].val;
    case GETNCNT:
        if (semnum < 0 || semnum >= st->nsems)
            return -E_RANGE;
        return st->sems[semnum].ncnt;
    case GETZCNT:
        if (semnum < 0 || semnum >= st->nsems)
            return -E_RANGE;
        return st->sems[semnum].zcnt;
    case SETVAL: {
        if (semnum < 0 || semnum >= st->nsems)
            return -E_RANGE;
        int r = ipc_permitted(&st->perm, 2);
        if (r < 0)
            return r;
        int val;
        if (!user_ptr_ok(uarg, sizeof(int)))
            return -E_FAULT;
        memcpy(&val, (const void *)(uintptr_t)uarg, sizeof(int));
        st->sems[semnum].val = val;
        st->sems[semnum].pid = 0;
        ipc_wake_all(&st->wake);
        return 0;
    }
    case GETALL: {
        if (!user_ptr_ok(uarg, (uint64_t)st->nsems * sizeof(uint16_t)))
            return -E_FAULT;
        int r = ipc_permitted(&st->perm, 1);
        if (r < 0)
            return r;
        uint16_t *arr = (uint16_t *)(uintptr_t)uarg;
        for (int i = 0; i < st->nsems; i++)
            arr[i] = (uint16_t)st->sems[i].val;
        return 0;
    }
    case SETALL: {
        if (!user_ptr_ok(uarg, (uint64_t)st->nsems * sizeof(uint16_t)))
            return -E_FAULT;
        int r = ipc_permitted(&st->perm, 2);
        if (r < 0)
            return r;
        const uint16_t *arr = (const uint16_t *)(uintptr_t)uarg;
        for (int i = 0; i < st->nsems; i++) {
            st->sems[i].val = arr[i];
            st->sems[i].pid = 0;
        }
        ipc_wake_all(&st->wake);
        return 0;
    }
    default:
        return -E_INVAL;        /* SEM_INFO/SEM_STAT variants not exposed */
    }
}

/* Roll back every SEM_UNDO adjustment of `pid` across all sets (process
 * exit).  Called from sysv_exit under no locks: user syscalls are under the
 * BKL, and exit runs in the exiting process's own context. */
static void sem_undo_all(int32_t pid)
{
    for (int i = 0; i < SEMSET_MAX; i++) {
        ksem_set_t *st = &g_semset[i];
        if (!st->used)
            continue;
        semundo_t **pp = &st->undos;
        while (*pp) {
            semundo_t *u = *pp;
            if (u->pid != pid) {
                pp = &u->next;
                continue;
            }
            for (int s = 0; s < st->nsems; s++) {
                if (u->adj[s])
                    st->sems[s].val += u->adj[s];
            }
            *pp = u->next;
            kfree(u);
        }
        if (st->undos || st->sems)
            ipc_wake_all(&st->wake);
    }
}

/* ------------------------------------------------------------------ */
/* shared memory                                                       */
/* ------------------------------------------------------------------ */

#define SHM_MAX   64
#define SHM_MAX_BYTES (16ULL << 20)      /* shmmax cap for one segment */

typedef struct {
    int32_t pid;                /* attached process, -1 = free row */
    uint64_t va;                /* address the segment is mapped at */
} shm_att_t;

typedef struct {
    int         used;
    uint16_t    seq;
    int32_t     key;
    kipc_perm_t perm;
    uint64_t    segsz;          /* bytes (page multiple) */
    uint64_t   *frames;         /* segsz/4096 physical frames (heap) */
    uint32_t    npages;
    int         destroy;        /* IPC_RMID seen; free when nattch == 0 */
    shm_att_t   att[64];        /* one row per possible process slot */
    int32_t     nattch;
    int32_t     cpid, lpid;
    int64_t     atime, dtime, ctime;
} kshm_t;

static kshm_t g_shm[SHM_MAX];

static kshm_t *shm_at(int shmid)
{
    int slot = shmid & ((1 << ID_SHIFT) - 1);
    if (slot < 0 || slot >= SHM_MAX || !g_shm[slot].used)
        return NULL;
    if ((uint32_t)(shmid >> ID_SHIFT) != g_shm[slot].seq)
        return NULL;
    return &g_shm[slot];
}

static int shm_alloc_slot(void)
{
    static uint16_t s_seq;
    for (int i = 0; i < SHM_MAX; i++) {
        if (g_shm[i].used)
            continue;
        g_shm[i].used = 1;
        g_shm[i].seq  = ++s_seq;
        return i;
    }
    return -1;
}

long sysv_shmget(int32_t key, size_t size, int flags)
{
    for (int i = 0; i < SHM_MAX; i++) {
        if (!g_shm[i].used || key == IPC_PRIVATE ||
            g_shm[i].key != key)
            continue;
        if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL))
            return -E_EXIST;
        int r = ipc_permitted(&g_shm[i].perm, 1);
        if (r < 0)
            return r;
        if (size > g_shm[i].segsz)
            return -E_INVAL;
        return make_id(g_shm[i].seq, i);
    }
    if (!(flags & IPC_CREAT))
        return -E_NOENT;
    if (size == 0 || size > SHM_MAX_BYTES)
        return -E_INVAL;

    uint64_t bytes = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint32_t npages = (uint32_t)(bytes >> 12);
    int slot = shm_alloc_slot();
    if (slot < 0)
        return -E_NFILE;
    kshm_t *s = &g_shm[slot];
    proc_t *p = proc_current();
    memset(s, 0, sizeof(*s));
    s->key    = key;
    s->perm.key  = key;
    s->perm.mode = (uint32_t)(flags & 0777);
    s->perm.uid = s->perm.cuid = p ? p->euid : 0;
    s->perm.gid = s->perm.cgid = p ? p->egid : 0;
    s->segsz  = bytes;
    s->npages = npages;
    s->cpid   = p ? p->pid : 0;
    s->ctime  = ipc_time();
    for (int i = 0; i < 64; i++)
        s->att[i].pid = -1;

    s->frames = kmalloc(sizeof(uint64_t) * npages);
    if (!s->frames) {
        s->used = 0;
        return -E_NOMEM;
    }
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t f = pmm_alloc_zeroed();
        if (!f) {
            while (i > 0)
                pmm_free(s->frames[--i]);
            kfree(s->frames);
            s->used = 0;
            return -E_NOMEM;
        }
        s->frames[i] = f;
    }
    return make_id(s->seq, slot);
}

/* Free the segment's frames and slot.  Caller guarantees nattch == 0. */
static void shm_release(kshm_t *s, int slot)
{
    for (uint32_t i = 0; i < s->npages; i++)
        pmm_free(s->frames[i]);
    kfree(s->frames);
    s->used = 0;
    s->frames = NULL;
    (void)slot;
}

/* Find a free span in the mmap arena for an unattached segment: everything
 * the process already mapped is recorded in as->mmaps, so scan those. */
static uint64_t shm_pick_base(proc_t *p, uint64_t size)
{
    addrspace_t *as = p->as;
    uint64_t base = USER_MMAP_BASE;
    for (int i = 0; i < as->nmmaps; i++) {
        if (as->mmaps[i].base < USER_MMAP_BASE ||
            as->mmaps[i].base >= USER_MMAP_CEIL)
            continue;
        uint64_t top = as->mmaps[i].base + as->mmaps[i].size;
        if (top > base)
            base = top;
    }
    if (base + size > USER_MMAP_CEIL)
        return 0;
    return base;
}

static int shm_record(proc_t *p, uint64_t base, uint64_t size)
{
    addrspace_t *as = p->as;
    if (as->nmmaps >= (int)(sizeof(as->mmaps) / sizeof(as->mmaps[0])))
        return 0;
    as->mmaps[as->nmmaps].base  = base;
    as->mmaps[as->nmmaps].size  = size;
    as->mmaps[as->nmmaps].flags = VM_USER | VM_WRITE | VM_EXTSHM;
    as->nmmaps++;
    return 1;
}

static void shm_forget(proc_t *p, uint64_t base)
{
    addrspace_t *as = p->as;
    for (int i = 0; i < as->nmmaps; i++) {
        if (as->mmaps[i].base != base)
            continue;
        for (int j = i; j < as->nmmaps - 1; j++)
            as->mmaps[j] = as->mmaps[j + 1];
        as->nmmaps--;
        return;
    }
}

/* Detach segment `s` (slot `slot`) from process `pid` whose mapping starts
 * at `va`.  Returns 1 if the segment was destroyed by the last detach. */
static int shm_detach_one(kshm_t *s, int slot, int32_t pid, uint64_t va)
{
    proc_t *p = proc_by_pid(pid);
    if (p && p->as) {
        vmm_unmap(p->as, va, s->segsz);     /* marked pages: frames kept */
        shm_forget(p, va);
    }
    s->nattch--;
    s->dtime = ipc_time();
    s->lpid  = pid;
    for (int i = 0; i < 64; i++) {
        if (s->att[i].pid == pid && s->att[i].va == va) {
            s->att[i].pid = -1;
            break;
        }
    }
    if (s->destroy && s->nattch <= 0) {
        shm_release(s, slot);
        return 1;
    }
    return 0;
}

long sysv_shmat(int shmid, uint64_t uaddr, int flags)
{
    kshm_t *s = shm_at(shmid);
    if (!s)
        return -E_INVAL;

    /* Attach permission: read for SHM_RDONLY, read+write otherwise. */
    int r = (flags & SHM_RDONLY) ? ipc_permitted(&s->perm, 1)
                                 : ipc_permitted(&s->perm, 2);
    if (r < 0)
        return r;

    proc_t *p = proc_current();
    if (!p || !p->as)
        return -E_INVAL;

    /* Already attached at the same address?  Linux rejects a duplicate
     * attach unless SHM_REMAP (which we do not support). */
    for (int i = 0; i < 64; i++) {
        if (s->att[i].pid == p->pid) {
            if (uaddr == 0 || s->att[i].va == (uaddr & ~0xFFFULL))
                return -E_INVAL;        /* duplicate attach */
            return -E_INVAL;
        }
    }

    uint64_t va = uaddr;
    if (flags & SHM_RND)
        va &= ~(uint64_t)(PAGE_SIZE - 1);
    if (va == 0)
        va = shm_pick_base(p, s->segsz);
    else
        va &= ~(uint64_t)(PAGE_SIZE - 1);
    if (!va || va + s->segsz > USER_LIMIT)
        return -E_INVAL;

    unsigned vf = VM_USER | VM_EXTSHM;
    if (!(flags & SHM_RDONLY))
        vf |= VM_WRITE;

    /* Map the segment's own frames (never freed by address-space teardown:
     * VM_EXTSHM pages carry a PTE marker bit). */
    for (uint32_t i = 0; i < s->npages; i++) {
        if (!vmm_map(p->as, va + ((uint64_t)i << 12), s->frames[i], vf)) {
            /* Roll back what we mapped so far. */
            vmm_unmap(p->as, va, (uint64_t)i << 12);
            return -E_NOMEM;
        }
    }
    if (!shm_record(p, va, s->segsz)) {
        vmm_unmap(p->as, va, s->segsz);
        return -E_NOMEM;
    }

    int row = -1;
    for (int i = 0; i < 64; i++) {
        if (s->att[i].pid == -1) {
            row = i;
            break;
        }
    }
    if (row < 0) {                      /* can't happen: 64 rows, 64 procs */
        vmm_unmap(p->as, va, s->segsz);
        shm_forget(p, va);
        return -E_MFILE;
    }
    s->att[row].pid = p->pid;
    s->att[row].va  = va;
    s->nattch++;
    s->atime = ipc_time();
    s->lpid  = p->pid;
    return (long)va;
}

long sysv_shmdt(uint64_t uaddr)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;
    uint64_t va = uaddr & ~(uint64_t)(PAGE_SIZE - 1);

    for (int i = 0; i < SHM_MAX; i++) {
        kshm_t *s = &g_shm[i];
        if (!s->used)
            continue;
        for (int k = 0; k < 64; k++) {
            if (s->att[k].pid == p->pid && s->att[k].va == va) {
                shm_detach_one(s, i, p->pid, va);
                return 0;
            }
        }
    }
    return -E_INVAL;                    /* not attached at that address */
}

long sysv_shmctl(int shmid, int cmd, uint64_t ubuf)
{
    kshm_t *s = shm_at(shmid);
    if (!s)
        return -E_INVAL;

    switch (cmd) {
    case IPC_RMID: {
        int r = ipc_permitted(&s->perm, 2);
        if (r < 0)
            return r;
        s->destroy = 1;
        s->perm.mode |= 01000u;         /* SHM_DEST visible to ipcs */
        if (s->nattch <= 0) {
            for (int i = 0; i < SHM_MAX; i++)
                if (&g_shm[i] == s) {
                    shm_release(s, i);
                    break;
                }
        }
        return 0;
    }
    case IPC_STAT: {
        if (!user_ptr_ok(ubuf, sizeof(kshmid_ds_t)))
            return -E_FAULT;
        int r = ipc_permitted(&s->perm, 1);
        if (r < 0)
            return r;
        kshmid_ds_t ds;
        memset(&ds, 0, sizeof(ds));
        memcpy(&ds.perm, &s->perm, sizeof(ds.perm));
        ds.segsz  = s->segsz;
        ds.atime  = s->atime;
        ds.dtime  = s->dtime;
        ds.ctime  = s->ctime;
        ds.cpid   = s->cpid;
        ds.lpid   = s->lpid;
        ds.nattch = (uint64_t)s->nattch;
        memcpy((void *)(uintptr_t)ubuf, &ds, sizeof(ds));
        return 0;
    }
    case IPC_SET: {
        if (!user_ptr_ok(ubuf, sizeof(kshmid_ds_t)))
            return -E_FAULT;
        proc_t *p = proc_current();
        if (p && p->euid != 0 &&
            p->euid != s->perm.uid && p->euid != s->perm.cuid)
            return -E_PERM;
        kshmid_ds_t ds;
        memcpy(&ds, (const void *)(uintptr_t)ubuf, sizeof(ds));
        s->perm.uid  = ds.perm.uid;
        s->perm.gid  = ds.perm.gid;
        s->perm.mode = ds.perm.mode & 0777u;
        return 0;
    }
    default:
        return -E_INVAL;
    }
}

/* ---- process-exit bookkeeping ------------------------------------------ */

/* Detach every segment this process has attached (called from sysv_exit and
 * from execve, which builds a fresh address space). */
static void shm_detach_all(int32_t pid)
{
    for (int i = 0; i < SHM_MAX; i++) {
        kshm_t *s = &g_shm[i];
        if (!s->used)
            continue;
        for (int k = 0; k < 64; k++) {
            if (s->att[k].pid != pid)
                continue;
            uint64_t va = s->att[k].va;
            if (s->nattch <= 0)
                continue;
            shm_detach_one(s, i, pid, va);
            break;                      /* one mapping per (pid, segment) */
        }
    }
}

void sysv_exit(proc_t *p)
{
    if (!p)
        return;
    shm_detach_all(p->pid);
    sem_undo_all(p->pid);
}

void sysv_exec_reset(proc_t *p)
{
    if (!p)
        return;
    /* The old address space is about to be replaced: every segment
     * mapping dies with it.  SEM_UNDO adjustments survive (Linux keeps
     * them across exec). */
    shm_detach_all(p->pid);
}
