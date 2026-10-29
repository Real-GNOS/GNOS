/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cgroup.c - Linux-style control groups, v2 (unified) hierarchy. (GPLv2)
 *
 * The implementation is split by responsibility:
 *   cgroup.c       tree, weights, membership, procfs ABI
 *   cgroup_cpu.c   cpu.max accounting and throttling
 *   cgroup_mem.c   hierarchical memory charge/discharge
 *   cgroup_fs.c    cgroupfs directories and control files
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

#include "cgroup_internal.h"
#include "kstring.h"

cgroup_t g_cgs[MAX_CGS];
int      g_nlive; /* live cgroups, for /proc/cgroups */

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

/* Direct member tasks of cg (g_procs scan; caller holds the lock). */
int cg_member_count(int cg)
{
    return g_cgs[cg].nprocs;
}

/* Live tasks in the whole subtree (pids.current; caller holds the lock). */
int cg_subtree_tasks_locked(int cg)
{
    int n = cg_member_count(cg);
    for (int c = g_cgs[cg].first_child; c != -1; c = g_cgs[c].next_sibling)
        n += cg_subtree_tasks_locked(c);
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
    int  parent = g_cgs[cg].parent;
    char pbuf[GNUOS_PATH_MAX];
    int  plen = render_path(parent, pbuf, (int)sizeof(pbuf));
    if (plen <= 0)
        return plen;
    int need = plen + 1 + (int)strlen(g_cgs[cg].name) + 1;
    if (need > cap)
        return -E_NOSPC;
    int o = 0;
    if (plen > 1) { /* "/a" -> copy "a"; root adds nothing */
        memcpy(buf, pbuf + 1, (size_t)(plen - 1));
        o = plen - 1;
    }
    buf[o++]      = '/';
    const char *n = g_cgs[cg].name;
    while (*n)
        buf[o++] = *n++;
    buf[o] = 0;
    return o;
}

int cg_path(int cg, char *buf, int cap)
{
    sched_lock();
    int r = (cg_live(cg) || cg == CG_ROOT) ? render_path(cg, buf, cap) : -E_NOENT;
    sched_unlock();
    return r;
}
/* ------------------------------------------------------------------ */
/* Controllers and weights (caller holds the sched lock)              */
/* ------------------------------------------------------------------ */

/* Is controller `bit` usable on cg?  It must be enabled in the
 * subtree_control of every ancestor from cg's parent up to (and including)
 * the root.  The root itself always sees every controller. */
int cg_ctrl_usable(int cg, uint32_t bit)
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
            if (cg_ctrl_usable(cg, CG_CTRL_BIT(k)))
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
    uint32_t w = (cg_live(cg) && cg != CG_ROOT) ? g_cgs[cg].weight : CG_WEIGHT_DEFAULT;
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
    int      cur = cg;
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
void cg_refresh_weights_locked(void)
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
    cg_refresh_weights_locked();
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
        if (g_cgs[c].pids_max >= 0 && cg_subtree_tasks_locked(c) >= g_cgs[c].pids_max) {
            extern void dbg_puts(const char *);
            extern void dbg_puts_dec(uint32_t);
            dbg_puts("PIDS: deny c=");
            dbg_puts_dec((uint32_t)c);
            dbg_puts(" max=");
            dbg_puts_dec((uint32_t)g_cgs[c].pids_max);
            dbg_puts(" tasks=");
            dbg_puts_dec((uint32_t)cg_subtree_tasks_locked(c));
            dbg_puts("\r\n");
            return -E_AGAIN;
        }
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
        p->cg           = cg;
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
    p->cg           = -1;
    p->cg_park_next = -1;
    sched_unlock();
}

/* Lock-free body of cg_move_tgid; caller holds the sched lock. */
int cg_move_tgid_locked(int pid, int cg)
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
        return 0; /* already all here */

    /* The destination chain must fit `moving` more tasks. */
    for (int c = cg; c != -1; c = g_cgs[c].parent) {
        if (g_cgs[c].pids_max >= 0 && cg_subtree_tasks_locked(c) + moving > g_cgs[c].pids_max)
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
    int r = cg_move_tgid_locked(pid, cg);
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
    int n = cg_live(cg) ? cg_subtree_tasks_locked(cg) : 0;
    sched_unlock();
    return n;
}
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
        int  plen = render_path(p->cg, path, (int)sizeof(path));
        if (plen < 0) {
            sched_unlock();
            return plen;
        }
        const char *prefix = "0::";
        int         need   = 3 + plen + 2; /* "0::" + path + '\n' + NUL */
        if (need > cap) {
            sched_unlock();
            return -E_NOSPC;
        }
        memcpy(buf, prefix, 3);
        memcpy(buf + 3, path, (size_t)plen);
        buf[3 + plen]     = '\n';
        buf[3 + plen + 1] = 0;
        len               = 3 + plen + 1;
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

    cgroup_t *root     = &g_cgs[CG_ROOT];
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
