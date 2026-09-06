/*
 * cgroup.c — Linux-style control groups, v2 (unified) hierarchy. (GPLv2)
 *
 * One global tree of cgroups.  Directories in the mounted cgroupfs create
 * child cgroups; control files in a directory expose the state of the cpu,
 * pids and memory controllers for the tasks in that cgroup's subtree.
 *
 * Controller semantics follow cgroup v2:
 *   - controllers are enabled level by level through cgroup.subtree_control
 *     ("+cpu", "-pids", ...).  The root starts with every controller enabled
 *     (a convenience: on Linux the root starts empty and the init system
 *     enables them after moving itself into a leaf), so the tree is usable
 *     the moment /sys/fs/cgroup appears.  Disabling one recursively clears
 *     it from every descendant's subtree_control, and controller files
 *     disappear from a directory once the controller is no longer usable
 *     on it (enabled all the way down from the root).
 *   - enabling a controller on a non-root cgroup is refused while that
 *     cgroup still holds member tasks (the "no internal processes" rule).
 *   - the pids controller counts every live task in a subtree and refuses
 *     forks/moves past pids.max with -E_AGAIN, exactly like Linux.
 *   - the cpu controller has both halves Linux v2 has: cpu.weight is a
 *     hierarchical share that the scheduler turns into proportional CPU
 *     time, and cpu.max is a quota/period cap enforced by throttling.
 *     See task #4 for how the EEVDF charge hook consumes it.
 *   - the memory controller tracks resident bytes of member address spaces
 *     through hierarchical charge/uncharge hooks in the VMM: every page
 *     allocated for a cgroup member is charged up the ancestor chain, and
 *     every page freed is discharged.  memory.max is enforced at charge
 *     time -- an allocation that would push any ancestor past its limit is
 *     refused with -ENOMEM.  memory.current reads the per-cgroup counter
 *     directly (no proc-table scan).  memory.stat reports the anon/file
 *     breakdown maintained alongside the byte counter.
 *
 * Locking: all cgroup state lives under the scheduler's run-queue lock
 * (g_proc_lock, taken through sched_lock()/sched_unlock()).  The scheduler
 * charge hooks run with that lock already held, so they never take it
 * themselves; every entry point from outside (the cgroupfs node ops, the
 * procfs generators, mount-time init) takes it for the duration of its
 * work.  Nothing under this lock may sleep or take the BKL.
 */
#include <stdint.h>

#include "cgroup.h"
#include "kstring.h"
#include "timer.h"
#include "vmm.h"

/* ------------------------------------------------------------------ */
/* The tree                                                           */
/* ------------------------------------------------------------------ */

typedef struct cgroup {
    int      parent;          /* cg slot; -1 for the root */
    int      first_child;     /* child slots, singly linked */
    int      next_sibling;
    char     name[CG_NAME_MAX + 1];

    uint32_t weight;          /* cpu.weight, 1..10000 */
    uint32_t subtree_ctl;     /* controllers enabled for our children */

    /* ---- cpu accounting (real ticks; SCHED_HZ = 100) ----------------- */
    uint64_t usage_total;     /* lifetime CPU of the subtree, for cpu.stat */
    uint64_t used_period;     /* CPU used since period_start */
    uint64_t period_start;    /* tick the current period began on */
    uint32_t period_ticks;    /* period length, in ticks */
    uint64_t quota_ticks;     /* cpu.max quota in ticks; 0 = unlimited */
    int      throttled;       /* quota exhausted until the period rolls over */
    uint64_t throttle_start;  /* tick throttling began */
    uint64_t throttled_total; /* ticks spent throttled, for cpu.stat */
    uint32_t nr_throttled;    /* times the quota has been exhausted */

    /* ---- pids --------------------------------------------------------- */
    int      nprocs;          /* direct member tasks */
    int      pids_max;        /* subtree task cap; -1 = unlimited */

    /* ---- memory (real accounting with hierarchical charging) ---------- */
    uint64_t mem_max;         /* bytes; ~0ULL = unlimited */
    uint64_t mem_bytes;       /* directly charged bytes (this cgroup only;
                               * subtree total = sum of all descendants) */
    uint64_t mem_anon;        /* anonymous pages charged (for memory.stat) */
    uint64_t mem_file;        /* file-backed pages charged (for memory.stat) */

    /* Tasks parked (WAIT_CGROUP) until this cgroup's period rolls over.
     * pid-based singly linked list through proc_t.cg_park_next. */
    int      park_head;

    int      live;
} cgroup_t;

static cgroup_t g_cgs[MAX_CGS];
static int      g_nlive;      /* live cgroups, for /proc/cgroups */

/* ------------------------------------------------------------------ */
/* Small text writers (numbers into a caller scratch buffer)          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    int   cap;
    int   len;
} cgbuf_t;

static void cg_char(cgbuf_t *b, char c)
{
    if (b->len < b->cap - 1)
        b->buf[b->len++] = c;
}

static void cg_str(cgbuf_t *b, const char *s)
{
    while (*s)
        cg_char(b, *s++);
}

static void cg_u64(cgbuf_t *b, uint64_t v)
{
    char tmp[24];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        cg_char(b, tmp[--n]);
}

/* ------------------------------------------------------------------ */
/* Raw tree walks (caller holds the sched lock)                       */
/* ------------------------------------------------------------------ */

int cg_live(int cg)
{
    return cg >= 0 && cg < MAX_CGS && g_cgs[cg].live;
}

int cg_parent(int cg)
{
    return cg_live(cg) ? g_cgs[cg].parent : -1;
}

int cg_child(int cg, int i)
{
    if (!cg_live(cg))
        return -1;
    int c = g_cgs[cg].first_child;
    while (c != -1 && i-- > 0)
        c = g_cgs[c].next_sibling;
    return (c != -1 && g_cgs[c].live) ? c : -1;
}

int cg_count(void)
{
    return g_nlive;
}

/* Number of live descendants (cgroup.stat's nr_descendants). */
static int count_descendants(int cg)
{
    int n = 0;
    for (int c = g_cgs[cg].first_child; c != -1; c = g_cgs[c].next_sibling)
        n += 1 + count_descendants(c);
    return n;
}

/* Direct member tasks of cg (g_procs scan; caller holds the lock). */
static int member_count(int cg)
{
    return g_cgs[cg].nprocs;
}

/* Live tasks in the whole subtree (pids.current; caller holds the lock). */
static int subtree_tasks(int cg)
{
    int n = member_count(cg);
    for (int c = g_cgs[cg].first_child; c != -1; c = g_cgs[c].next_sibling)
        n += subtree_tasks(c);
    return n;
}

/* Full path of cg ("/", "/a", "/a/b") into buf.  Caller holds the lock. */
static int render_path(int cg, char *buf, int cap)
{
    /* Recurse up, then copy the components down in order.  The root is the
     * only cgroup whose path is just "/". */
    if (cg == CG_ROOT) {
        if (cap > 0) {
            buf[0] = '/';
            if (cap > 1)
                buf[1] = 0;
        }
        return 1;
    }
    int parent = g_cgs[cg].parent;
    char pbuf[GNUOS_PATH_MAX];
    int  plen = render_path(parent, pbuf, (int)sizeof(pbuf));
    if (plen <= 0)
        return plen;
    int need = plen + 1 + (int)strlen(g_cgs[cg].name) + 1;
    if (need > cap)
        return -E_NOSPC;
    int o = 0;
    if (plen > 1) {                 /* "/a" -> copy "a"; root adds nothing */
        memcpy(buf, pbuf + 1, (size_t)(plen - 1));
        o = plen - 1;
    }
    buf[o++] = '/';
    const char *n = g_cgs[cg].name;
    while (*n)
        buf[o++] = *n++;
    buf[o] = 0;
    return o;
}

int cg_path(int cg, char *buf, int cap)
{
    sched_lock();
    int r = (cg_live(cg) || cg == CG_ROOT)
                ? render_path(cg, buf, cap) : -E_NOENT;
    sched_unlock();
    return r;
}

/* ------------------------------------------------------------------ */
/* Controllers and weights (caller holds the sched lock)              */
/* ------------------------------------------------------------------ */

/* Is controller `bit` usable on cg?  It must be enabled in the
 * subtree_control of every ancestor from cg's parent up to (and including)
 * the root.  The root itself always sees every controller. */
static int ctrl_usable(int cg, uint32_t bit)
{
    if (cg == CG_ROOT)
        return 1;
    for (int c = g_cgs[cg].parent; c != -1; c = g_cgs[c].parent) {
        if (!(g_cgs[c].subtree_ctl & bit))
            return 0;
        if (c == CG_ROOT)
            break;
    }
    return 1;
}

uint32_t cg_controllers(int cg)
{
    sched_lock();
    uint32_t m = 0;
    if (cg_live(cg)) {
        for (int k = 0; k < CG_NCTRLS; k++)
            if (ctrl_usable(cg, CG_CTRL_BIT(k)))
                m |= CG_CTRL_BIT(k);
    }
    sched_unlock();
    return m;
}

uint32_t cg_subtree(int cg)
{
    sched_lock();
    uint32_t m = cg_live(cg) ? g_cgs[cg].subtree_ctl : 0;
    sched_unlock();
    return m;
}

uint32_t cg_weight(int cg)
{
    sched_lock();
    uint32_t w = (cg_live(cg) && cg != CG_ROOT) ? g_cgs[cg].weight
                                                : CG_WEIGHT_DEFAULT;
    sched_unlock();
    return w;
}

/* Hierarchical effective weight of cg, in CG_NICE0 units.  Each level of
 * the tree that has the cpu controller enabled splits that level's share of
 * the CPU among its children in proportion to their weights, so a task's
 * effective weight is the product of the (child-weight / siblings-sum)
 * ratios along its path -- the same decomposition Linux computes with
 * calc_group_shares().  Caller holds the sched lock. */
static uint32_t eff_weight(int cg)
{
    if (!cg_live(cg))
        return CG_NICE0;

    uint64_t eff = CG_NICE0;
    int cur = cg;
    while (cur != CG_ROOT) {
        int pa = g_cgs[cur].parent;
        if (pa == -1 || (g_cgs[pa].subtree_ctl & CG_CTRL_BIT(CG_CTRL_CPU))) {
            /* Sibling weights are relative to each other at this level;
             * a lone child inherits its parent's whole share. */
            uint64_t sum = 0;
            for (int s = g_cgs[pa].first_child; s != -1; s = g_cgs[s].next_sibling)
                if (g_cgs[s].live)
                    sum += g_cgs[s].weight;
            if (sum == 0)
                sum = 1;
            eff = eff * g_cgs[cur].weight / sum;
            if (eff < CG_EFF_MIN)
                eff = CG_EFF_MIN;
            if (eff > CG_EFF_MAX)
                eff = CG_EFF_MAX;
        }
        cur = pa;
    }
    return (uint32_t)eff;
}

uint32_t cg_eff_weight(int cg)
{
    sched_lock();
    uint32_t w = eff_weight(cg);
    sched_unlock();
    return w;
}

/* Recompute the cached scheduler weight of every live task.  Cheap enough
 * to run after any topology/weight change: one pass over the proc table,
 * each entry a short walk up its cgroup chain.  Caller holds the lock. */
static void refresh_all_weights(void)
{
    for (int i = 0; i < proc_capacity(); i++) {
        proc_t *p = proc_at(i);
        if (p->state == PROC_UNUSED || p->cg < 0)
            continue;
        p->sched_weight = eff_weight(p->cg);
    }
}

void cg_refresh_weights(void)
{
    sched_lock();
    refresh_all_weights();
    sched_unlock();
}

/* ------------------------------------------------------------------ */
/* Membership (caller holds the sched lock)                           */
/* ------------------------------------------------------------------ */

/* Enforce pids.max along cg's whole ancestor chain: adding one task to cg
 * also adds it to every ancestor's subtree.  Returns -E_AGAIN when any
 * level is at its cap. */
static int pids_allow_one(int cg)
{
    for (int c = cg; c != -1; c = g_cgs[c].parent) {
        if (g_cgs[c].pids_max >= 0 &&
            subtree_tasks(c) >= g_cgs[c].pids_max)
            return -E_AGAIN;
        if (c == CG_ROOT)
            break;
    }
    return 0;
}

int cg_attach_new(proc_t *p, int cg)
{
    sched_lock();
    int r;
    if (!p || p->cg != -1 || !cg_live(cg)) {
        r = -E_INVAL;
    } else if ((r = pids_allow_one(cg)) != 0) {
        /* keep p unattached */
    } else {
        p->cg = cg;
        p->sched_weight = eff_weight(cg);
        g_cgs[cg].nprocs++;
        /* Propagate the cgroup to the address space so the VMM memory
         * controller can charge page allocations against the right tree. */
        if (p->as)
            p->as->cg = cg;
        r = 0;
    }
    sched_unlock();
    return r;
}

void cg_detach(proc_t *p)
{
    if (!p)
        return;
    sched_lock();
    if (p->cg >= 0 && p->cg < MAX_CGS && g_cgs[p->cg].live) {
        if (g_cgs[p->cg].nprocs > 0)
            g_cgs[p->cg].nprocs--;
    }
    p->cg = -1;
    p->cg_park_next = -1;
    sched_unlock();
}

/* Lock-free body of cg_move_tgid; caller holds the sched lock. */
static int move_tgid_locked(int pid, int cg)
{
    if (!cg_live(cg))
        return -E_NOENT;

    /* Writing a thread id moves the whole thread group; find its leader. */
    proc_t *leader = NULL;
    for (int i = 0; i < proc_capacity(); i++) {
        proc_t *q = proc_at(i);
        if (q->state == PROC_UNUSED)
            continue;
        if (q->pid == pid) {
            leader = q;
            break;
        }
    }
    if (!leader)
        return -E_SRCH;
    int tgid = leader->tgid;

    /* Count how many live members of the group will actually move. */
    int moving = 0;
    for (int i = 0; i < proc_capacity(); i++) {
        proc_t *q = proc_at(i);
        if (q->state == PROC_UNUSED || q->tgid != tgid)
            continue;
        if (q->cg >= 0 && q->cg != cg)
            moving++;
    }
    if (moving == 0)
        return 0;               /* already all here */

    /* The destination chain must fit `moving` more tasks. */
    for (int c = cg; c != -1; c = g_cgs[c].parent) {
        if (g_cgs[c].pids_max >= 0 &&
            subtree_tasks(c) + moving > g_cgs[c].pids_max)
            return -E_AGAIN;
        if (c == CG_ROOT)
            break;
    }

    for (int i = 0; i < proc_capacity(); i++) {
        proc_t *q = proc_at(i);
        if (q->state == PROC_UNUSED || q->tgid != tgid)
            continue;
        if (q->cg >= 0 && q->cg != cg) {
            g_cgs[q->cg].nprocs--;
            q->cg = cg;
            g_cgs[cg].nprocs++;
        }
        q->sched_weight = eff_weight(cg);
        /* Propagate the new cgroup to shared address spaces. */
        if (q->as)
            q->as->cg = cg;
    }
    return 0;
}

int cg_move_tgid(int pid, int cg)
{
    sched_lock();
    int r = move_tgid_locked(pid, cg);
    sched_unlock();
    return r;
}

int cg_member_pid(int cg, int i)
{
    sched_lock();
    int found = -1;
    if (cg_live(cg)) {
        for (int k = 0; k < proc_capacity(); k++) {
            proc_t *q = proc_at(k);
            if (q->state == PROC_UNUSED || q->cg != cg)
                continue;
            if (i-- == 0) {
                found = q->pid;
                break;
            }
        }
    }
    sched_unlock();
    return found;
}

int cg_subtree_tasks(int cg)
{
    sched_lock();
    int n = cg_live(cg) ? subtree_tasks(cg) : 0;
    sched_unlock();
    return n;
}

/* ------------------------------------------------------------------ */
/* cpu controller: accounting, quota, throttling (lock held by the    */
/* scheduler charge sites, or by cgfs entry points)                   */
/* ------------------------------------------------------------------ */

/* Roll any finished periods of cg over; releases tasks parked on a period
 * that just ended.  Caller holds the sched lock. */
static void period_rollover(cgroup_t *g, uint64_t now)
{
    if (g->quota_ticks == 0 || g->period_ticks == 0)
        return;
    while (now >= g->period_start + g->period_ticks) {
        if (g->throttled) {
            uint64_t end = g->period_start + g->period_ticks;
            g->throttled_total += (end > g->throttle_start)
                                      ? end - g->throttle_start : 0;
            g->throttled = 0;
        }
        g->period_start += g->period_ticks;
        g->used_period   = 0;
    }
}

/* Release (or re-park, if an ancestor is still throttled) every task parked
 * on cg's list.  Caller holds the sched lock. */
static void release_parked(int cg)
{
    int pid = g_cgs[cg].park_head;
    g_cgs[cg].park_head = -1;
    while (pid != -1) {
        proc_t *p = proc_by_pid(pid);
        int next = p ? p->cg_park_next : -1;
        if (p && p->state == PROC_BLOCKED && p->wait_reason == WAIT_CGROUP) {
            p->cg_park_next = -1;
            /* Any ancestor still throttled?  Park one level higher. */
            int anchor = -1;
            for (int c = p->cg; c != -1; c = g_cgs[c].parent) {
                if (g_cgs[c].throttled)
                    anchor = c;      /* keep the root-most one */
                if (c == CG_ROOT)
                    break;
            }
            if (anchor != -1) {
                p->cg_park_next = g_cgs[anchor].park_head;
                g_cgs[anchor].park_head = p->pid;
            } else {
                sched_enqueue(p);    /* make runnable again */
            }
        }
        pid = next;
    }
}

/* Whole-tree periodic sweep, called from every timer tick.  Catches quota
 * periods that ended while the machine was idle (charge only happens while
 * a task runs, but parked tasks must be released even then). */
void cg_tick_refresh(void)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < MAX_CGS; i++) {
        cgroup_t *g = &g_cgs[i];
        if (!g->live || g->quota_ticks == 0)
            continue;
        if (g->throttled && now >= g->period_start + g->period_ticks) {
            period_rollover(g, now);
            release_parked(i);
        }
    }
}

int cg_chain_throttled(proc_t *p)
{
    if (!p || p->cg < 0)
        return 0;
    for (int c = p->cg; c != -1; c = g_cgs[c].parent) {
        if (g_cgs[c].throttled)
            return 1;
        if (c == CG_ROOT)
            break;
    }
    return 0;
}

void cg_park(proc_t *p)
{
    if (!p || p->cg < 0)
        return;
    p->state      = PROC_BLOCKED;
    p->wait_reason = WAIT_CGROUP;
    p->cg_park_next = -1;

    /* Anchor on the root-most throttled ancestor: that is the last one to
     * un-throttle, so it is the single list we must be released from. */
    int anchor = -1;
    for (int c = p->cg; c != -1; c = g_cgs[c].parent) {
        if (g_cgs[c].throttled)
            anchor = c;
        if (c == CG_ROOT)
            break;
    }
    if (anchor == -1) {
        /* Race: the period rolled over between the check and the park.
         * Just make the task runnable again. */
        sched_enqueue(p);
        return;
    }
    p->cg_park_next = g_cgs[anchor].park_head;
    g_cgs[anchor].park_head = p->pid;
}

/*
 * Charge `used` real ticks of CPU time to every cgroup on p's path.  Keeps
 * cpu.stat's lifetime counter and enforces cpu.max: once a period's quota
 * is gone the cgroup is marked throttled and the caller (who is about to
 * re-enqueue or keep running p) should hand the CPU back -- the enqueue
 * path parks throttled tasks.  Returns 1 when the chain is throttled.
 */
int cg_charge_runtime(proc_t *p, uint64_t used)
{
    if (!p || p->cg < 0 || used == 0)
        return 0;
    uint64_t now = timer_ticks();
    int throttled = 0;

    for (int c = p->cg; c != -1; c = g_cgs[c].parent) {
        cgroup_t *g = &g_cgs[c];
        if (now >= g->period_start + g->period_ticks)
            period_rollover(g, now);
        g->usage_total += used;
        if (g->quota_ticks) {
            g->used_period += used;
            if (!g->throttled && g->used_period > g->quota_ticks) {
                g->throttled = 1;
                g->nr_throttled++;
                g->throttle_start = now;
            }
            if (g->throttled)
                throttled = 1;
        }
        if (c == CG_ROOT)
            break;
    }
    return throttled;
}

/* ------------------------------------------------------------------ */
/* memory controller: hierarchical charge / discharge with enforcement */
/* ------------------------------------------------------------------ */

/* Check whether charging `bytes` to the subtree rooted at `cg` would push
 * any ancestor (including cg itself) past its memory.max.  Returns 0 when
 * the charge is allowed, -ENOMEM when it is not.  The check walks the
 * ancestor chain and reads the current mem_bytes at each level; since the
 * BKL is held and the kernel is single-CPU, no concurrent charge can slip
 * in between the check and the actual write in cg_mem_charge(). */
static int mem_limit_check(int cg, uint64_t bytes)
{
    for (int c = cg; c != -1; c = g_cgs[c].parent) {
        if (g_cgs[c].mem_bytes + bytes > g_cgs[c].mem_max)
            return -ENOMEM;
        if (c == CG_ROOT)
            break;
    }
    return 0;
}

/* Charge `bytes` to every cgroup on cg's ancestor chain.  Returns 0 on
 * success, -ENOMEM when any level would exceed its memory.max (no partial
 * charge is applied in that case).  Caller holds the BKL. */
int cg_mem_charge(int cg, uint64_t bytes)
{
    if (cg < 0 || cg >= MAX_CGS || !g_cgs[cg].live || bytes == 0)
        return 0;

    if (mem_limit_check(cg, bytes) < 0)
        return -ENOMEM;

    for (int c = cg; c != -1; c = g_cgs[c].parent) {
        g_cgs[c].mem_bytes += bytes;
        if (c == CG_ROOT)
            break;
    }
    return 0;
}

/* Discharge `bytes` from every cgroup on cg's ancestor chain.  Unlike
 * charging this never fails -- freeing memory is always allowed.  Caller
 * holds the BKL. */
void cg_mem_discharge(int cg, uint64_t bytes)
{
    if (cg < 0 || cg >= MAX_CGS || !g_cgs[cg].live || bytes == 0)
        return;

    for (int c = cg; c != -1; c = g_cgs[c].parent) {
        if (g_cgs[c].mem_bytes >= bytes)
            g_cgs[c].mem_bytes -= bytes;
        else
            g_cgs[c].mem_bytes = 0;       /* shouldn't happen; clamp */
        if (c == CG_ROOT)
            break;
    }
}

/* ==== cgroupfs (resolve/readdir/control files) ========================= */

/* The well-known control files of a cgroup directory.  The always-present
 * ones are the v2 core files; the controller files exist only while that
 * controller is usable on the directory (enabled from the root). */
enum {
    F_PROCS, F_CONTROLLERS, F_SUBTREE, F_EVENTS, F_STAT,
    F_CPU_WEIGHT, F_CPU_MAX, F_CPU_STAT,
    F_PIDS_MAX, F_PIDS_CURRENT,
    F_MEM_CURRENT, F_MEM_MAX, F_MEM_STAT,
    F_MAX
};

typedef struct {
    const char *name;
    uint8_t     file;           /* F_* */
    uint8_t     ctrl;           /* CG_NCTRLS == always present */
} cgfile_t;

static const cgfile_t g_files[] = {
    { "cgroup.procs",        F_PROCS,          CG_NCTRLS },
    { "cgroup.controllers",  F_CONTROLLERS,    CG_NCTRLS },
    { "cgroup.subtree_control", F_SUBTREE,     CG_NCTRLS },
    { "cgroup.events",       F_EVENTS,         CG_NCTRLS },
    { "cgroup.stat",         F_STAT,           CG_NCTRLS },
    { "cpu.weight",          F_CPU_WEIGHT,     CG_CTRL_CPU },
    { "cpu.max",             F_CPU_MAX,        CG_CTRL_CPU },
    { "cpu.stat",            F_CPU_STAT,       CG_CTRL_CPU },
    { "pids.max",            F_PIDS_MAX,       CG_CTRL_PIDS },
    { "pids.current",        F_PIDS_CURRENT,   CG_CTRL_PIDS },
    { "memory.current",      F_MEM_CURRENT,    CG_CTRL_MEM },
    { "memory.max",          F_MEM_MAX,        CG_CTRL_MEM },
    { "memory.stat",         F_MEM_STAT,       CG_CTRL_MEM },
};
#define NFILES ((int)(sizeof(g_files) / sizeof(g_files[0])))

/* Encode (cg, file) in the node's priv pointer: the pool is small, the
 * shift keeps both fields in 32 bits and 0 stays an invalid id. */
#define PRIV(cg, f)  ((void *)(uintptr_t)((((cg) + 1) << 8) | (f)))
#define PRIV_CG(p)   ((int)(((uintptr_t)(p) >> 8) - 1))
#define PRIV_FILE(p) ((int)((uintptr_t)(p) & 0xFF))

/* Directory reads are EISDIR; the children come back through getdents64,
 * which the VFS routes to cgfs_readdir() by mount path. */
static int32_t cgdir_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static const vfs_ops_t g_cgdir_ops = { .read = cgdir_read, .write = NULL };
/* Defined with cgfile_read/cgfile_write below; forward-declared so the
 * resolver above can reference it. */
static const vfs_ops_t g_cgfile_ops;

/* ---- rendering --------------------------------------------------------- */
static void render_file(int cg, int f, cgbuf_t *b)
{
    cgroup_t *g = &g_cgs[cg];
    switch (f) {
    case F_PROCS:
        /* Thread-group leaders that are direct members, ascending. */
        for (int i = 0; i < proc_capacity(); i++) {
            proc_t *q = proc_at(i);
            if (q->state == PROC_UNUSED || q->cg != cg || q->pid != q->tgid)
                continue;
            cg_u64(b, (uint64_t)q->pid);
            cg_char(b, '\n');
        }
        break;
    case F_CONTROLLERS: {
        for (int k = 0; k < CG_NCTRLS; k++) {
            if (!ctrl_usable(cg, CG_CTRL_BIT(k)))
                continue;
            if (b->len > 0 && b->buf[b->len - 1] != '\n' && b->buf[b->len - 1] != ' ')
                cg_char(b, ' ');
            cg_str(b, (k == CG_CTRL_CPU) ? "cpu"
                     : (k == CG_CTRL_PIDS) ? "pids" : "memory");
        }
        cg_char(b, '\n');
        break;
    }
    case F_SUBTREE:
        for (int k = 0; k < CG_NCTRLS; k++) {
            if (!(g->subtree_ctl & CG_CTRL_BIT(k)))
                continue;
            cg_char(b, '+');
            cg_str(b, (k == CG_CTRL_CPU) ? "cpu" : (k == CG_CTRL_PIDS) ? "pids"
                                                                      : "memory");
            cg_char(b, ' ');
        }
        if (b->len > 0 && b->buf[b->len - 1] == ' ')
            b->len--;
        cg_char(b, '\n');
        break;
    case F_EVENTS:
        cg_str(b, "populated ");
        cg_u64(b, subtree_tasks(cg) > 0 ? 1 : 0);
        cg_str(b, "\nfrozen 0\n");
        break;
    case F_STAT:
        cg_str(b, "nr_descendants ");
        cg_u64(b, count_descendants(cg));
        cg_str(b, "\nnr_dying_descendants 0\n");
        break;
    case F_CPU_WEIGHT:
        cg_u64(b, g->weight);
        cg_char(b, '\n');
        break;
    case F_CPU_MAX:
        if (g->quota_ticks == 0)
            cg_str(b, "max ");
        else {
            cg_u64(b, g->quota_ticks * 10000ULL);       /* ticks -> usec */
            cg_char(b, ' ');
        }
        cg_u64(b, g->period_ticks * 10000ULL);
        cg_char(b, '\n');
        break;
    case F_CPU_STAT:
        cg_str(b, "usage_usec ");
        cg_u64(b, g->usage_total * 10000ULL);
        cg_str(b, "\nthrottled_usec ");
        cg_u64(b, g->throttled_total * 10000ULL);
        cg_str(b, "\nnr_throttled ");
        cg_u64(b, g->nr_throttled);
        cg_char(b, '\n');
        break;
    case F_PIDS_MAX:
        if (g->pids_max < 0)
            cg_str(b, "max\n");
        else {
            cg_u64(b, (uint64_t)g->pids_max);
            cg_char(b, '\n');
        }
        break;
    case F_PIDS_CURRENT:
        cg_u64(b, subtree_tasks(cg));
        cg_char(b, '\n');
        break;
    case F_MEM_CURRENT:
        cg_u64(b, g->mem_bytes);
        cg_char(b, '\n');
        break;
    case F_MEM_MAX:
        if (g->mem_max == ~0ULL)
            cg_str(b, "max\n");
        else {
            cg_u64(b, g->mem_max);
            cg_char(b, '\n');
        }
        break;
    case F_MEM_STAT:
        cg_str(b, "anon ");
        cg_u64(b, g->mem_anon);
        cg_str(b, "\nfile ");
        cg_u64(b, g->mem_file);
        cg_str(b, "\n");
        break;
    default:
        break;
    }
}

/* ---- file read/write node ops ------------------------------------------ */

static int32_t cgfile_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    static char scratch[1024];

    int cg = PRIV_CG(n->priv);
    int f  = PRIV_FILE(n->priv);

    /* The scratch buffer is shared, so the render and the copy out both
     * happen under the sched lock. */
    sched_lock();
    if (!cg_live(cg)) {
        sched_unlock();
        return -E_NOENT;
    }
    cgbuf_t b = { scratch, (int)sizeof(scratch), 0 };
    render_file(cg, f, &b);
    uint32_t size = (uint32_t)b.len;
    uint32_t copied = 0;
    if (off < size) {
        uint32_t avail = size - (uint32_t)off;
        if (len > avail)
            len = avail;
        memcpy(buf, scratch + off, len);
        copied = len;
    }
    sched_unlock();
    return (int32_t)copied;
}

/* Skip leading whitespace and parse the next unsigned token; `max` maps to
 * ~0ULL.  Returns 0 on success and advances *pp past the token. */
static int parse_u64(const char **pp, uint64_t *out)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (p[0] == 'm' && p[1] == 'a' && p[2] == 'x' &&
        (p[3] == 0 || p[3] == ' ' || p[3] == '\n' || p[3] == '\r')) {
        *out = ~0ULL;
        *pp = p + 3;
        return 0;
    }
    if (*p < '0' || *p > '9')
        return -E_INVAL;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        p++;
    }
    *out = v;
    *pp = p;
    return 0;
}

static int parse_s32(const char **pp, int *out)
{
    uint64_t v;
    int r = parse_u64(pp, &v);
    if (r < 0)
        return r;
    if (v == ~0ULL)
        return -E_INVAL;
    *out = (int)v;
    return 0;
}

/* Apply a write to a control file.  The whole buffer is one logical write
 * regardless of the file offset, as on Linux. */
static int apply_write(int cg, int f, const char *data, uint32_t len)
{
    cgroup_t *g = &g_cgs[cg];
    const char *p = data;
    const char *end = data + len;
    (void)end;

    switch (f) {
    case F_PROCS: {
        /* One or more pids (thread-group leaders or members); each moves
         * that task's whole thread group into cg. */
        for (;;) {
            while (p < data + len && (*p == ' ' || *p == '\t' ||
                                      *p == '\r' || *p == '\n'))
                p++;
            if (p >= data + len)
                break;
            uint64_t pid;
            if (parse_u64(&p, &pid) < 0)
                return -E_INVAL;
            if (pid > 0x7FFFFFFFULL)
                return -E_INVAL;
            int r = move_tgid_locked((int)pid, cg);   /* lock already held */
            if (r < 0)
                return r;
        }
        return (int)len;
    }
    case F_SUBTREE: {
        uint32_t want = g->subtree_ctl;
        for (;;) {
            while (p < data + len && (*p == ' ' || *p == '\t' ||
                                      *p == '\r' || *p == '\n'))
                p++;
            if (p >= data + len)
                break;
            if (*p != '+' && *p != '-')
                return -E_INVAL;
            int on = (*p == '+');
            p++;
            uint32_t bit;
            if (strncmp(p, "cpu", 3) == 0)
                bit = CG_CTRL_BIT(CG_CTRL_CPU), p += 3;
            else if (strncmp(p, "pids", 4) == 0)
                bit = CG_CTRL_BIT(CG_CTRL_PIDS), p += 4;
            else if (strncmp(p, "memory", 6) == 0)
                bit = CG_CTRL_BIT(CG_CTRL_MEM), p += 6;
            else
                return -E_INVAL;
            if (on) {
                if (!ctrl_usable(cg, bit))
                    return -E_INVAL;      /* enable top-down first */
                /* "no internal processes": a cgroup with member tasks may
                 * not hand controllers down to its children (the root is
                 * exempt so the tree can be re-armed after a boot-time
                 * "-cpu" without moving init out of the way). */
                if (cg != CG_ROOT && member_count(cg) > 0)
                    return -E_BUSY;
                want |= bit;
            } else {
                want &= ~bit;
            }
        }
        g->subtree_ctl = want;
        refresh_all_weights();
        return (int)len;
    }
    case F_CPU_WEIGHT: {
        int w;
        if (parse_s32(&p, &w) < 0)
            return -E_INVAL;
        if (w < CG_WEIGHT_MIN || w > CG_WEIGHT_MAX)
            return -E_RANGE;
        g->weight = (uint32_t)w;
        refresh_all_weights();
        return (int)len;
    }
    case F_CPU_MAX: {
        uint64_t quota, period;
        if (parse_u64(&p, &quota) < 0)
            return -E_INVAL;
        if (parse_u64(&p, &period) < 0)
            return -E_INVAL;
        if (period == 0 || period == ~0ULL)
            return -E_INVAL;
        if (period < 10000)                 /* one tick minimum */
            period = 10000;
        uint32_t ticks = (uint32_t)((period + 9999) / 10000);
        if (ticks > 10000)                  /* cap at ~100 s */
            return -E_RANGE;
        g->period_ticks = ticks;
        g->quota_ticks  = (quota == ~0ULL)
                              ? 0
                              : (uint64_t)((quota + 9999) / 10000);
        if (g->quota_ticks == 0)
            g->quota_ticks = 1;             /* a real quota, never "off" */
        /* A new, larger period may already be over: sync now. */
        period_rollover(g, timer_ticks());
        return (int)len;
    }
    case F_PIDS_MAX: {
        int v;
        if (parse_s32(&p, &v) < 0)
            return -E_INVAL;
        g->pids_max = (v < 0) ? -1 : v;
        return (int)len;
    }
    case F_MEM_MAX: {
        uint64_t v;
        if (parse_u64(&p, &v) < 0)
            return -E_INVAL;
        g->mem_max = v;                     /* stored; no enforcement */
        return (int)len;
    }
    default:
        return -E_ROFS;
    }
}

static int32_t cgfile_write(vfs_node_t *n, uint64_t off, const void *buf,
                            uint32_t len)
{
    (void)off;
    int cg = PRIV_CG(n->priv);
    int f  = PRIV_FILE(n->priv);
    if (!cg_live(cg))
        return -E_NOENT;
    if (len == 0)
        return 0;
    /* The buffer must be usable as a C string for the token parsers; a
     * control-file write never exceeds one small line. */
    if (len > 255)
        return -E_INVAL;
    char tmp[256];
    memcpy(tmp, buf, len);
    tmp[len] = 0;

    sched_lock();
    int r = apply_write(cg, f, tmp, len);
    sched_unlock();
    return (r < 0) ? r : (int32_t)len;
}

/* ---- lookup ------------------------------------------------------------- */

static int name_to_file(int cg, const char *name, int *file_out)
{
    for (int i = 0; i < NFILES; i++) {
        if (g_files[i].ctrl != CG_NCTRLS &&
            !ctrl_usable(cg, CG_CTRL_BIT(g_files[i].ctrl)))
            continue;
        if (strcmp(g_files[i].name, name) == 0) {
            *file_out = i;
            return 1;
        }
    }
    return 0;
}

/* Split `rel` into components and walk the tree to the cgroup directory
 * named by everything but the last component.  Returns the cg slot, or -1.
 * When the last component is a control file of that directory, *file_out
 * receives its index and the return is still the directory's cg.  Caller
 * holds the sched lock. */
static int walk_rel(const char *rel, char *leaf, int leaf_cap, int *file_out)
{
    if (!rel || rel[0] != '/')
        return -1;
    if (file_out)
        *file_out = -1;

    int cg = CG_ROOT;
    const char *p = rel + 1;
    for (;;) {
        if (*p == 0)
            return cg;              /* exactly a directory */
        while (*p == '/')
            p++;
        if (*p == 0)
            return cg;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        int n = (int)(p - start);
        if (n > CG_NAME_MAX || n <= 0)
            return -1;

        /* A further component follows: this must be a child cgroup. */
        if (*p == '/') {
            int child = -1;
            for (int c = g_cgs[cg].first_child; c != -1; c = g_cgs[c].next_sibling)
                if (g_cgs[c].live &&
                    (int)strlen(g_cgs[c].name) == n &&
                    memcmp(g_cgs[c].name, start, (size_t)n) == 0) {
                    child = c;
                    break;
                }
            if (child == -1)
                return -1;
            cg = child;
            continue;
        }

        /* Last component: a control file of cg, or nothing. */
        char tmp[CG_NAME_MAX + 1];
        memcpy(tmp, start, (size_t)n);
        tmp[n] = 0;
        int idx;
        if (name_to_file(cg, tmp, &idx)) {
            if (leaf && leaf_cap > 0) {
                memcpy(leaf, tmp, (size_t)n + 1);
            }
            if (file_out)
                *file_out = idx;
            return cg;
        }
        return -1;
    }
}

int cgfs_resolve(const char *rel, vfs_node_t *out)
{
    if (!out)
        return -E_INVAL;
    memset(out, 0, sizeof(*out));

    sched_lock();
    char leaf[CG_NAME_MAX + 1];
    int  file = -1;
    int  cg = walk_rel(rel, leaf, (int)sizeof(leaf), &file);
    if (cg < 0 || !cg_live(cg)) {
        sched_unlock();
        return -E_NOENT;
    }
    if (file >= 0) {
        strncpy(out->name, g_files[file].name, VFS_NAME_MAX - 1);
        out->kind = VFS_FILE;
        out->size = 0;
        out->ops  = &g_cgfile_ops;
        out->priv = PRIV(cg, g_files[file].file);
    } else {
        strncpy(out->name, leaf[0] ? leaf : "cgroup", VFS_NAME_MAX - 1);
        out->kind = VFS_DIR;
        out->ops  = &g_cgdir_ops;
    }
    sched_unlock();
    return 0;
}

int cgfs_readdir(const char *rel, uint32_t index, char *name, uint8_t *type)
{
    sched_lock();
    int cg = walk_rel(rel, NULL, 0, NULL);
    if (cg < 0 || !cg_live(cg)) {
        sched_unlock();
        return -E_NOTDIR;
    }

    uint32_t n = 0;
    if (index == n++) {
        strncpy(name, ".", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        sched_unlock();
        return 0;
    }
    if (index == n++) {
        strncpy(name, "..", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        sched_unlock();
        return 0;
    }

    /* Child cgroups first, then the control files. */
    for (int c = g_cgs[cg].first_child; c != -1; c = g_cgs[c].next_sibling) {
        if (!g_cgs[c].live)
            continue;
        if (index == n++) {
            strncpy(name, g_cgs[c].name, VFS_NAME_MAX - 1);
            *type = DT_DIR;
            sched_unlock();
            return 0;
        }
    }
    for (int i = 0; i < NFILES; i++) {
        if (g_files[i].ctrl != CG_NCTRLS &&
            !ctrl_usable(cg, CG_CTRL_BIT(g_files[i].ctrl)))
            continue;
        if (index == n++) {
            strncpy(name, g_files[i].name, VFS_NAME_MAX - 1);
            *type = DT_REG;
            sched_unlock();
            return 0;
        }
    }
    sched_unlock();
    return -E_NOENT;
}

/* ------------------------------------------------------------------ */
/* mkdir / rmdir                                                      */
/* ------------------------------------------------------------------ */

static int name_ok(const char *name)
{
    if (!name || name[0] == 0)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/')
            return 0;
    }
    return (int)strlen(name) <= CG_NAME_MAX;
}

int cgfs_mkdir(const char *rel)
{
    if (!rel || rel[0] != '/')
        return -E_INVAL;

    /* Split off the final component: the parent directory must exist and
     * the child name must be a fresh one. */
    const char *slash = rel;
    for (const char *p = rel; *p; p++)
        if (*p == '/')
            slash = p;
    if (slash == rel)
        return -E_EXIST;            /* "mkdir /" */

    char parent_rel[GNUOS_PATH_MAX];
    int  plen = (int)(slash - rel);
    if (plen == 0) {
        strncpy(parent_rel, "/", sizeof(parent_rel) - 1);
    } else {
        memcpy(parent_rel, rel, (size_t)plen);
        parent_rel[plen] = 0;
    }
    const char *name = slash + 1;
    if (!name_ok(name))
        return -E_INVAL;

    sched_lock();
    int cg = walk_rel(parent_rel, NULL, 0, NULL);
    if (cg < 0 || !cg_live(cg)) {
        sched_unlock();
        return -E_NOENT;
    }
    for (int c = g_cgs[cg].first_child; c != -1; c = g_cgs[c].next_sibling)
        if (g_cgs[c].live && strcmp(g_cgs[c].name, name) == 0) {
            sched_unlock();
            return -E_EXIST;
        }

    /* A fresh static slot. */
    int slot = -1;
    for (int i = CG_ROOT + 1; i < MAX_CGS; i++)
        if (!g_cgs[i].live) {
            slot = i;
            break;
        }
    if (slot == -1) {
        sched_unlock();
        return -E_NOSPC;            /* static pool exhausted (never reused) */
    }

    cgroup_t *g = &g_cgs[slot];
    memset(g, 0, sizeof(*g));
    strncpy(g->name, name, CG_NAME_MAX);
    g->live          = 1;
    g->parent        = cg;
    g->first_child   = -1;
    g->next_sibling  = g_cgs[cg].first_child;
    g_cgs[cg].first_child = slot;
    g->weight        = CG_WEIGHT_DEFAULT;
    g->period_ticks  = CG_PERIOD_DEFAULT_TICKS;
    g->quota_ticks   = 0;
    g->pids_max      = -1;
    g->mem_max       = ~0ULL;
    g->park_head     = -1;
    g_nlive++;
    refresh_all_weights();
    sched_unlock();
    return 0;
}

int cgfs_rmdir(const char *rel)
{
    if (!rel || rel[0] != '/' || strcmp(rel, "/") == 0)
        return -E_INVAL;

    sched_lock();
    int cg = walk_rel(rel, NULL, 0, NULL);
    if (cg < 0 || !cg_live(cg)) {
        sched_unlock();
        return -E_NOENT;
    }
    if (cg == CG_ROOT) {
        sched_unlock();
        return -E_BUSY;
    }
    if (member_count(cg) > 0 || g_cgs[cg].first_child != -1 ||
        g_cgs[cg].subtree_ctl != 0 || g_cgs[cg].park_head != -1) {
        sched_unlock();
        return -E_BUSY;             /* not empty (tasks/children/controllers) */
    }
    g_cgs[cg].live = 0;
    g_nlive--;

    /* Unlink from the parent's child list. */
    int parent = g_cgs[cg].parent;
    int *link = &g_cgs[parent].first_child;
    while (*link != -1) {
        if (*link == cg) {
            *link = g_cgs[cg].next_sibling;
            break;
        }
        link = &g_cgs[*link].next_sibling;
    }
    refresh_all_weights();
    sched_unlock();
    return 0;
}

/* ------------------------------------------------------------------ */
/* init and procfs helpers                                            */
/* ------------------------------------------------------------------ */

static const vfs_ops_t g_cgfile_ops = { .read = cgfile_read, .write = cgfile_write };

/* cgroup v2 exposes one hierarchy; /proc/cgroups reports the single
 * hierarchy with all three controllers on it. */
int cg_proc_cgroup_line(proc_t *p, char *buf, int cap)
{
    if (!p || cap <= 0)
        return -E_INVAL;
    sched_lock();
    int len;
    if (p->cg >= 0 && cg_live(p->cg)) {
        char path[GNUOS_PATH_MAX];
        int plen = render_path(p->cg, path, (int)sizeof(path));
        if (plen < 0) {
            sched_unlock();
            return plen;
        }
        const char *prefix = "0::";
        int need = 3 + plen + 2;        /* "0::" + path + '\n' + NUL */
        if (need > cap) {
            sched_unlock();
            return -E_NOSPC;
        }
        memcpy(buf, prefix, 3);
        memcpy(buf + 3, path, (size_t)plen);
        buf[3 + plen] = '\n';
        buf[3 + plen + 1] = 0;
        len = 3 + plen + 1;
    } else {
        /* unattached (zombie): report the root */
        if (cap < 4) {
            sched_unlock();
            return -E_NOSPC;
        }
        memcpy(buf, "0::/\n", 5);
        len = 4;
    }
    sched_unlock();
    return len;
}

int cgroup_init(void)
{
    memset(g_cgs, 0, sizeof(g_cgs));

    cgroup_t *root = &g_cgs[CG_ROOT];
    root->parent       = -1;
    root->first_child  = -1;
    root->next_sibling = -1;
    root->name[0]      = 0;
    root->weight       = CG_WEIGHT_DEFAULT;
    /* The root starts with every controller enabled for its children so the
     * hierarchy is usable immediately (see the file header). */
    root->subtree_ctl  = CG_ALL_CTRLS;
    root->period_ticks = CG_PERIOD_DEFAULT_TICKS;
    root->quota_ticks  = 0;
    root->pids_max     = -1;
    root->mem_max      = ~0ULL;
    root->park_head    = -1;
    root->live         = 1;
    g_nlive            = 1;
    return 0;
}

