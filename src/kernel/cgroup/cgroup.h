/*
 * cgroup.h — Linux-style control groups, v2 (unified) layout. (GPLv2)
 *
 * A cgroup is a node in a single global hierarchy whose filesystem view
 * ("cgroup2") is mounted at /sys/fs/cgroup and mountable anywhere else by
 * name.  Directories create child cgroups; the well-known files in each
 * directory carry the controller state:
 *
 *     cgroup.procs            rw   move a thread group here by PID
 *     cgroup.controllers      ro   controllers usable on this cgroup
 *     cgroup.subtree_control  rw   +cpu / -pids ... for this cgroup's children
 *     cgroup.events           ro   populated/frozen
 *     cgroup.stat             ro   nr_descendants / nr_dying_descendants
 *     cpu.weight              rw   hierarchical share weight (1..10000)
 *     cpu.max                 rw   quota/period bandwidth cap in usec
 *     cpu.stat                ro   usage_usec throttled_usec nr_throttled
 *     pids.max                rw   task-count cap ("max" = unlimited)
 *     pids.current            ro   tasks in this subtree
 *     memory.current          ro   resident bytes of member address spaces
 *     memory.max              rw   limit knob (accounting only, no reclaim)
 *     memory.stat             ro   anon/file breakdown (approximate)
 *
 * Controller files appear in a directory only while the controller is
 * enabled all the way down from the root (Linux's "no internal processes"
 * visibility rule is honoured for +cpu/-cpu subtree_control writes).
 *
 * The hierarchy is a fixed static pool, exactly like the proc table and the
 * tmpfs node pool: slots are never reused within a boot, so a vfs_node that
 * was resolved once never dangles, at the cost of leaking a few hundred
 * bytes per rmdir'd cgroup -- acceptable for one boot, and it keeps every
 * control file's priv pointer valid for as long as an fd holds it.
 *
 * Synchronisation is deliberately single-lock: every cgroup tree/state
 * mutation AND every read that renders state takes the scheduler's
 * g_proc_lock (through sched_lock()/sched_unlock() below).  The scheduler
 * already holds that lock at its charge sites, so the accounting helpers
 * here never take a lock themselves ("lock held by caller").  The one
 * per-page counter added to addrspace_t for the memory controller is
 * updated without the lock (aligned u32/u64 stores are atomic on x86-64),
 * which is good enough for statistics.
 */
#ifndef GNUCOS_CGROUP_H
#define GNUCOS_CGROUP_H

#include <stdint.h>
#include "vfs.h"
#include "proc.h"

/* ---- controllers -------------------------------------------------------- */
#define CG_CTRL_CPU    0
#define CG_CTRL_PIDS   1
#define CG_CTRL_MEM    2
#define CG_NCTRLS      3
#define CG_CTRL_BIT(n) (1u << (n))

#define CG_ALL_CTRLS (CG_CTRL_BIT(CG_CTRL_CPU) | \
                      CG_CTRL_BIT(CG_CTRL_PIDS) | \
                      CG_CTRL_BIT(CG_CTRL_MEM))

/* ---- tree limits -------------------------------------------------------- */
#define MAX_CGS       512      /* static pool; slots are never reused */
#define CG_ROOT       0        /* the root cgroup lives at slot 0 */
#define CG_NAME_MAX   31       /* component names (VFS_NAME_MAX is 32) */

/* ---- cpu controller ------------------------------------------------------ */
#define CG_WEIGHT_MIN     1
#define CG_WEIGHT_MAX     10000
#define CG_WEIGHT_DEFAULT 100

/* Effective weight of a default (weight 100) task, the NICE_0 anchor every
 * virtual-time accounting rate is scaled against. */
#define CG_NICE0         1024

/* Bounds on the hierarchical effective weight a task can end up with.
 * 16 is ~1/64 of a default task, 262144 is 256x one; both keep the
 * fixed-point virtual-time deltas at least 1 unit per scheduler tick and
 * stop a deep, hostile hierarchy from overflowing the u64 clocks. */
#define CG_EFF_MIN   16
#define CG_EFF_MAX   262144

/* Controller timers are quantised to scheduler ticks (SCHED_HZ = 100).
 * cpu.max defaults to unlimited; when a quota is written the period
 * defaults to 100000 usec == 10 ticks, the Linux default. */
#define CG_PERIOD_DEFAULT_TICKS 10

/* ---- exported tree shape (read-only) ------------------------------------ */
int  cgroup_init(void);
int  cg_count(void);                    /* live cgroups, for /proc/cgroups */

/* Enumerate a cgroup's children: return the child index at 0-based slot
 * `i`, or -1 when exhausted.  Caller holds the sched lock. */
int  cg_child(int cg, int i);
int  cg_parent(int cg);
int  cg_live(int cg);

/* Path of a cgroup ("/" for the root, "/a/b" otherwise) rendered into buf.
 * Returns the length.  Caller holds the sched lock. */
int  cg_path(int cg, char *buf, int cap);

/* Controllers currently usable on cg (enabled all the way from the root),
 * as a CG_CTRL_BIT mask; plus which of them are enabled for cg's own
 * children (cg's subtree_control).  Caller holds the sched lock. */
uint32_t cg_controllers(int cg);
uint32_t cg_subtree(int cg);

/* Weight and the effective (hierarchical) weight of a task in this cgroup;
 * used by the scheduler's charge hook.  Caller holds the sched lock. */
uint32_t cg_weight(int cg);
uint32_t cg_eff_weight(int cg);

/* ---- lifecycle ---------------------------------------------------------- */

/* Attach a freshly created process (never attached before) to cgroup `cg`.
 * Enforces the pids controller: returns 0, or -E_AGAIN when the subtree is
 * at its pids.max.  Caller holds the sched lock; p->cg must be -1. */
int  cg_attach_new(proc_t *p, int cg);

/* Detach a process that is leaving (exit).  Caller holds the sched lock;
 * p->cg is reset to -1. */
void cg_detach(proc_t *p);

/* Move the thread group containing pid `pid` into cgroup `cg`.  Returns 0
 * or a negative errno.  pids.max of the destination is enforced. */
int  cg_move_tgid(int pid, int cg);

/* Recompute every task's cached effective weight after a weight write,
 * mkdir/rmdir or subtree_control change.  Caller holds the sched lock. */
void cg_refresh_weights(void);

/* ---- scheduler hooks (caller holds g_proc_lock) -------------------------- */

/* Charge `used` real ticks of CPU time to every cgroup on p's path (the
 * chain from p's leaf to the root).  Maintains usage accounting and the
 * cpu.max budget; returns 1 when the leaf chain is throttled right now
 * (the caller should not leave p runnable), 0 otherwise. */
int  cg_charge_runtime(proc_t *p, uint64_t used);

/* Whether p's cgroup chain is currently throttled. */
int  cg_chain_throttled(proc_t *p);

/* Park a runnable-but-throttled task: mark it BLOCKED (WAIT_CGROUP) and
 * link it on its throttled cgroup's parked list so the period rollover can
 * release it.  Caller holds the sched lock. */
void cg_park(proc_t *p);

/* Roll over exhausted periods and release anything parked on them.  Called
 * from the timer tick with the sched lock held. */
void cg_tick_refresh(void);

/* ---- memory hooks (called from VMM fault/alloc/unmap paths) --------------- */

/* Charge `bytes` resident memory to every cgroup on `cg`'s ancestor chain.
 * Returns 0 on success, -ENOMEM when any ancestor would exceed its
 * memory.max.  Caller must hold the BKL (already held at every call site).
 * When this returns 0, the caller MUST later call cg_mem_discharge() with
 * the same bytes when the pages are freed. */
int  cg_mem_charge(int cg, uint64_t bytes);

/* Discharge (un-charge) `bytes` from every cgroup on `cg`'s ancestor chain.
 * Called when resident pages are freed or an address space is destroyed. */
void cg_mem_discharge(int cg, uint64_t bytes);

/* ---- cgroupfs ------------------------------------------------------------ */
/* Path relative to the cgroupfs mount root (always starts with '/') is
 * resolved/enumerated exactly like the tmpfs calls of the same shape. */
int  cgfs_resolve(const char *rel, vfs_node_t *out);
int  cgfs_readdir(const char *rel, uint32_t index, char *name, uint8_t *type);
int  cgfs_mkdir(const char *rel);
int  cgfs_rmdir(const char *rel);

/* ---- per-process queries (procfs) ---------------------------------------- */
/* cgroup path of process `p`, e.g. "0::/a/b" (the v2 unified layout of
 * /proc/<pid>/cgroup).  Rendered into buf; returns length. */
int  cg_proc_cgroup_line(proc_t *p, char *buf, int cap);
/* pid of the member task at 0-based slot `i` of cgroup `cg` (direct
 * members only), or -1.  Caller holds the sched lock. */
int  cg_member_pid(int cg, int i);
/* Total live tasks in the subtree rooted at `cg` (pids.current). */
int  cg_subtree_tasks(int cg);

#endif
