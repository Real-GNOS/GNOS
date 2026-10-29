/* SPDX-License-Identifier: GPL-2.0 */
/*
 * namespace.c — namespace architecture implementation. (GPLv2)
 *
 * Ported from the Uinxed process stack (kernel/process/namespace.c): the
 * seven initial namespaces, nsproxy reference handling, per-namespace
 * cloning on the CLONE_NEW* / unshare(2) flags, and the setns(2) entry
 * point.  Errnos are the GNOS names and the allocator is the kernel heap;
 * the cgroup namespace holds no root handle because GNOS tracks cgroup
 * membership as a leaf slot on proc_t.
 */
#include <stdbool.h>
#include <stdint.h>

#include "heap.h"
#include "kstring.h"
#include "namespace.h"
#include "proc.h"
#include "vfs.h"

#include "drm_vsnprintf.h" /* plogk */

uts_namespace_t init_uts_ns = {
    .nodename   = "(none)",
    .domainname = "(none)",
    .ns         = {.refcount = 1, .lock = {0}},
};

ipc_namespace_t init_ipc_ns = {
    .ns       = {.refcount = 1, .lock = {0}},
    .sysv_ids = NULL,
};

mnt_namespace_t init_mnt_ns = {
    .id         = 1,
    .ns         = {.refcount = 1, .lock = {0}},
    .root_mount = NULL,
};

pid_namespace_t init_pid_ns = {
    .level        = 0,
    .parent       = NULL,
    .pid_max      = 32768,
    .next_pid     = 1,
    .ns           = {.refcount = 1, .lock = {0}},
    .child_reaper = NULL,
    .dead         = false,
};

net_namespace_t init_net_ns = {
    .ns           = {.refcount = 1, .lock = {0}},
    .loopback_dev = NULL,
};

user_namespace_t init_user_ns = {
    .parent            = NULL,
    .owner_uid         = 0,
    .owner_gid         = 0,
    .uid_extent_count  = 1,
    .uid_map           = {{.first = 0, .lower_first = 0, .count = 4294967295U}},
    .gid_extent_count  = 1,
    .gid_map           = {{.first = 0, .lower_first = 0, .count = 4294967295U}},
    .setgroups_allowed = true,
    .ns                = {.refcount = 1, .lock = {0}},
};

cgroup_namespace_t init_cgroup_ns = {
    .root_cgroup = NULL,
    .ns          = {.refcount = 1, .lock = {0}},
};

nsproxy_t init_nsproxy = {
    .uts_ns    = &init_uts_ns,
    .ipc_ns    = &init_ipc_ns,
    .mnt_ns    = &init_mnt_ns,
    .pid_ns    = &init_pid_ns,
    .net_ns    = &init_net_ns,
    .user_ns   = &init_user_ns,
    .cgroup_ns = &init_cgroup_ns,
    .ns        = {.refcount = 1, .lock = {0}},
};

static uint64_t next_mnt_id = 2;

/* Namespace init. */
void namespace_init(void)
{
    plogk("namespace: 7 Linux namespaces initialized.\n");
}

/* Return the UTS namespace of the current process, or the initial one outside process context. */
uts_namespace_t *uts_namespace_current(void)
{
    proc_t *p = proc_current();
    if (p && p->nsproxy && p->nsproxy->uts_ns)
        return p->nsproxy->uts_ns;
    return &init_uts_ns;
}

/* Take a reference on a namespace. */
static void ns_ref_get(ns_common_t *common)
{
    spin_lock(&common->lock);
    common->refcount++;
    spin_unlock(&common->lock);
}

/* Drop a reference on a namespace; true when the last one is gone. */
static bool ns_ref_put(ns_common_t *common)
{
    bool release = false;
    spin_lock(&common->lock);
    if (--common->refcount == 0)
        release = true;
    spin_unlock(&common->lock);
    return release;
}

/* Nsproxy get. */
nsproxy_t *nsproxy_get(nsproxy_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Nsproxy put. */
void nsproxy_put(nsproxy_t *ns)
{
    if (!ns || ns == &init_nsproxy)
        return;
    if (!ns_ref_put(&ns->ns))
        return;

    uts_ns_put(ns->uts_ns);
    ipc_ns_put(ns->ipc_ns);
    mnt_ns_put(ns->mnt_ns);
    pid_ns_put(ns->pid_ns);
    net_ns_put(ns->net_ns);
    user_ns_put(ns->user_ns);
    cgroup_ns_put(ns->cgroup_ns);
    kfree(ns);
}

/* Individual namespace refcount handlers */

/* Uts ns get. */
uts_namespace_t *uts_ns_get(uts_namespace_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Uts ns put. */
void uts_ns_put(uts_namespace_t *ns)
{
    if (!ns || ns == &init_uts_ns)
        return;
    if (ns_ref_put(&ns->ns))
        kfree(ns);
}

/* IPC ns get. */
ipc_namespace_t *ipc_ns_get(ipc_namespace_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* IPC ns put. */
void ipc_ns_put(ipc_namespace_t *ns)
{
    if (!ns || ns == &init_ipc_ns)
        return;
    if (ns_ref_put(&ns->ns))
        kfree(ns);
}

/* Mnt ns get. */
mnt_namespace_t *mnt_ns_get(mnt_namespace_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Mnt ns put. */
void mnt_ns_put(mnt_namespace_t *ns)
{
    if (!ns || ns == &init_mnt_ns)
        return;
    if (ns_ref_put(&ns->ns))
        kfree(ns);
}

/* Pid ns get. */
pid_namespace_t *pid_ns_get(pid_namespace_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Pid ns put. */
void pid_ns_put(pid_namespace_t *ns)
{
    if (!ns || ns == &init_pid_ns)
        return;
    if (ns_ref_put(&ns->ns))
        kfree(ns);
}

/* Network helper: ns get. */
net_namespace_t *net_ns_get(net_namespace_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* Network helper: ns put. */
void net_ns_put(net_namespace_t *ns)
{
    if (!ns || ns == &init_net_ns)
        return;
    if (ns_ref_put(&ns->ns))
        kfree(ns);
}

/* User ns get. */
user_namespace_t *user_ns_get(user_namespace_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* User ns put. */
void user_ns_put(user_namespace_t *ns)
{
    if (!ns || ns == &init_user_ns)
        return;
    if (ns_ref_put(&ns->ns))
        kfree(ns);
}

/* cgroup ns get. */
cgroup_namespace_t *cgroup_ns_get(cgroup_namespace_t *ns)
{
    if (!ns)
        return NULL;
    ns_ref_get(&ns->ns);
    return ns;
}

/* cgroup ns put. */
void cgroup_ns_put(cgroup_namespace_t *ns)
{
    if (!ns || ns == &init_cgroup_ns)
        return;
    if (ns_ref_put(&ns->ns))
        kfree(ns);
}

/* Create new namespaces */
static uts_namespace_t *clone_uts_ns(uts_namespace_t *old)
{
    uts_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->ns.refcount = 1;
    if (old) {
        spin_lock(&old->ns.lock);
        memcpy(ns->nodename, old->nodename, sizeof(ns->nodename));
        memcpy(ns->domainname, old->domainname, sizeof(ns->domainname));
        spin_unlock(&old->ns.lock);
    } else {
        strncpy(ns->nodename, "(none)", sizeof(ns->nodename) - 1);
        strncpy(ns->domainname, "(none)", sizeof(ns->domainname) - 1);
    }
    return ns;
}

/* Clone a ipc ns namespace. */
static ipc_namespace_t *clone_ipc_ns(ipc_namespace_t *old)
{
    (void)old;
    ipc_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->ns.refcount = 1;
    return ns;
}

/* Clone a mnt ns namespace. */
static mnt_namespace_t *clone_mnt_ns(mnt_namespace_t *old)
{
    (void)old;
    mnt_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->id          = next_mnt_id++;
    ns->ns.refcount = 1;
    return ns;
}

/* Clone a pid ns namespace. */
static pid_namespace_t *clone_pid_ns(pid_namespace_t *old)
{
    pid_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->level       = old ? old->level + 1 : 1;
    ns->parent      = pid_ns_get(old);
    ns->pid_max     = 32768;
    ns->next_pid    = 1;
    ns->ns.refcount = 1;
    return ns;
}

/* Clone a net ns namespace. */
static net_namespace_t *clone_net_ns(net_namespace_t *old)
{
    (void)old;
    net_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->ns.refcount = 1;
    return ns;
}

/* Clone a user ns namespace. */
static user_namespace_t *clone_user_ns(user_namespace_t *old, proc_t *owner)
{
    user_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->parent            = user_ns_get(old);
    ns->owner_uid         = owner ? owner->uid : 0;
    ns->owner_gid         = owner ? owner->gid : 0;
    ns->setgroups_allowed = true;
    ns->ns.refcount       = 1;
    return ns;
}

/* Clone a cgroup ns namespace.  GNOS keeps no root cgroup handle (membership
 * is a leaf slot on proc_t), so the clone starts from the same empty root. */
static cgroup_namespace_t *clone_cgroup_ns(cgroup_namespace_t *old)
{
    (void)old;
    cgroup_namespace_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->ns.refcount = 1;
    return ns;
}

/* Clone nsproxy */
nsproxy_t *nsproxy_clone(nsproxy_t *orig, uint64_t flags, int *error)
{
    if (!orig)
        orig = &init_nsproxy;
    if (!(flags & (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER |
                   CLONE_NEWPID | CLONE_NEWNET))) {
        *error = 0;
        return nsproxy_get(orig);
    }

    nsproxy_t *ns = kcalloc(1, sizeof(*ns));
    if (!ns) {
        *error = -E_NOMEM;
        return NULL;
    }
    ns->ns.refcount = 1;

    proc_t *curr_p = proc_current();

    ns->uts_ns = (flags & CLONE_NEWUTS) ? clone_uts_ns(orig->uts_ns) : uts_ns_get(orig->uts_ns);
    ns->ipc_ns = (flags & CLONE_NEWIPC) ? clone_ipc_ns(orig->ipc_ns) : ipc_ns_get(orig->ipc_ns);
    ns->mnt_ns = (flags & CLONE_NEWNS) ? clone_mnt_ns(orig->mnt_ns) : mnt_ns_get(orig->mnt_ns);
    ns->pid_ns = (flags & CLONE_NEWPID) ? clone_pid_ns(orig->pid_ns) : pid_ns_get(orig->pid_ns);
    ns->net_ns = (flags & CLONE_NEWNET) ? clone_net_ns(orig->net_ns) : net_ns_get(orig->net_ns);
    ns->user_ns =
        (flags & CLONE_NEWUSER) ? clone_user_ns(orig->user_ns, curr_p) : user_ns_get(orig->user_ns);
    ns->cgroup_ns = (flags & CLONE_NEWCGROUP) ? clone_cgroup_ns(orig->cgroup_ns)
                                              : cgroup_ns_get(orig->cgroup_ns);

    if (!ns->uts_ns || !ns->ipc_ns || !ns->mnt_ns || !ns->pid_ns || !ns->net_ns || !ns->user_ns ||
        !ns->cgroup_ns) {
        nsproxy_put(ns);
        *error = -E_NOMEM;
        return NULL;
    }

    *error = 0;
    return ns;
}

/* Unshare specified namespaces for current task */
int namespace_unshare(uint64_t unshare_flags)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;

    int        error  = 0;
    nsproxy_t *new_ns = nsproxy_clone(p->nsproxy, unshare_flags, &error);
    if (error != 0)
        return error;

    nsproxy_t *old_ns = p->nsproxy;
    p->nsproxy        = new_ns;

    nsproxy_put(old_ns);
    return 0;
}

/* Switch namespace via fd.  Upstream only validates the descriptor so far;
 * per-type rebinding arrives with the per-namespace /proc/ns links. */
int namespace_setns(int fd, int nstype)
{
    (void)nstype;
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;
    if (fd < 0 || fd >= PROC_MAX_FD || p->fds[fd] < 0)
        return -E_BADF;
    return 0;
}
