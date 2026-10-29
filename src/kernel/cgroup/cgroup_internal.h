/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cgroup_internal.h - shared state for the cgroup core, controllers and
 * cgroupfs.  Not a public interface; include cgroup.h from callers outside
 * src/kernel/cgroup/.
 */
#ifndef GNOS_CGROUP_INTERNAL_H
#define GNOS_CGROUP_INTERNAL_H

#include <stdint.h>

#include "cgroup.h"

typedef struct cgroup {
    int  parent;      /* cg slot; -1 for the root */
    int  first_child; /* child slots, singly linked */
    int  next_sibling;
    char name[CG_NAME_MAX + 1];

    uint32_t weight;      /* cpu.weight, 1..10000 */
    uint32_t subtree_ctl; /* controllers enabled for our children */

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
    int nprocs;   /* direct member tasks */
    int pids_max; /* subtree task cap; -1 = unlimited */

    /* ---- memory (real accounting with hierarchical charging) ---------- */
    uint64_t mem_max;   /* bytes; ~0ULL = unlimited */
    uint64_t mem_bytes; /* directly charged bytes (this cgroup only;
                         * subtree total = sum of all descendants) */
    uint64_t mem_anon;  /* anonymous pages charged (for memory.stat) */
    uint64_t mem_file;  /* file-backed pages charged (for memory.stat) */

    /* Tasks parked (WAIT_CGROUP) until this cgroup's period rolls over.
     * pid-based singly linked list through proc_t.cg_park_next. */
    int park_head;

    int live;
} cgroup_t;

extern cgroup_t g_cgs[MAX_CGS];
extern int      g_nlive; /* live cgroups, for /proc/cgroups */

/* Core helpers.  All of these require the scheduler lock. */
int  cg_member_count(int cg);
int  cg_subtree_tasks_locked(int cg);
int  cg_ctrl_usable(int cg, uint32_t bit);
void cg_refresh_weights_locked(void);
int  cg_move_tgid_locked(int pid, int cg);

/* cpu controller helper for a cpu.max write; caller holds the lock. */
void cg_cpu_set_max(cgroup_t *g, uint64_t quota_ticks, uint32_t period_ticks);

#endif /* GNOS_CGROUP_INTERNAL_H */
