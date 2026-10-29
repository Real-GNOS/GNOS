/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cgroup_cpu.c - cgroup v2 cpu controller accounting, quota and throttling.
 *
 * cpu.weight is consumed by the scheduler through cg_eff_weight(); this file
 * owns cpu.max and its period rollover.  The charge hooks run with the
 * scheduler lock held by the caller.
 */
#include "cgroup_internal.h"
#include "timer.h"

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
            g->throttled_total += (end > g->throttle_start) ? end - g->throttle_start : 0;
            g->throttled = 0;
        }
        g->period_start += g->period_ticks;
        g->used_period = 0;
    }
}

/* Release (or re-park, if an ancestor is still throttled) every task parked
 * on cg's list.  Caller holds the sched lock. */
static void release_parked(int cg)
{
    int pid             = g_cgs[cg].park_head;
    g_cgs[cg].park_head = -1;
    while (pid != -1) {
        proc_t *p    = proc_by_pid(pid);
        int     next = p ? p->cg_park_next : -1;
        if (p && p->state == PROC_BLOCKED && p->wait_reason == WAIT_CGROUP) {
            p->cg_park_next = -1;
            /* Any ancestor still throttled?  Park one level higher. */
            int anchor = -1;
            for (int c = p->cg; c != -1; c = g_cgs[c].parent) {
                if (g_cgs[c].throttled)
                    anchor = c; /* keep the root-most one */
                if (c == CG_ROOT)
                    break;
            }
            if (anchor != -1) {
                p->cg_park_next         = g_cgs[anchor].park_head;
                g_cgs[anchor].park_head = p->pid;
            } else {
                sched_enqueue(p); /* make runnable again */
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
    p->state        = PROC_BLOCKED;
    p->wait_reason  = WAIT_CGROUP;
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
    p->cg_park_next         = g_cgs[anchor].park_head;
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
    uint64_t now       = timer_ticks();
    int      throttled = 0;

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

/* Apply a cpu.max write (quota/period already converted to ticks).  The
 * current period is rolled over immediately so a newly written limit takes
 * effect without waiting for the next tick.  Caller holds the sched lock. */
void cg_cpu_set_max(cgroup_t *g, uint64_t quota_ticks, uint32_t period_ticks)
{
    g->period_ticks = period_ticks;
    g->quota_ticks  = quota_ticks;
    if (g->quota_ticks == 0)
        g->quota_ticks = 1; /* a real quota, never "off" */
    period_rollover(g, timer_ticks());
}
