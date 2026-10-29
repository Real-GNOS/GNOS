/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cgroup_mem.c - cgroup v2 memory controller hierarchical charge/discharge.
 *
 * The VMM and fault paths call cg_mem_charge()/cg_mem_discharge() with the
 * BKL held.  memory.max is enforced at charge time by walking the ancestor
 * chain before any counter is modified.
 */
#include "cgroup_internal.h"

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
            g_cgs[c].mem_bytes = 0; /* shouldn't happen; clamp */
        if (c == CG_ROOT)
            break;
    }
}
