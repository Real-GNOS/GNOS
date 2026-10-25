/* SPDX-License-Identifier: GPL-2.0 */
/*
 * namespace.h — namespace architecture. (GPLv2)
 *
 * Ported from the Uinxed process stack (include/process/namespace.h): the
 * seven Linux namespaces (uts, ipc, mount, pid, net, user, cgroup), the
 * nsproxy bundle a process points at, and the refcount/clone/unshare
 * machinery around them.  Every namespace starts out shared from
 * init_nsproxy; clone(2) flags and unshare(2) create private copies.
 *
 * Deviation from the Uinxed original: the cgroup namespace carries no root
 * handle -- GNOS tracks cgroup membership as a leaf slot on proc_t
 * (cgroup.h) with nothing pointer-like to retain.
 */
#ifndef GNUCOS_NAMESPACE_H
#define GNUCOS_NAMESPACE_H

#include <stdbool.h>
#include <stdint.h>

#include "smp.h"

#define UID_GID_MAP_MAX 5 /* Max entries in a uid/gid map */

/* Namespace-isolation flags, Linux x86-64 values. */
#define CLONE_NEWNS     0x00020000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS    0x04000000
#define CLONE_NEWIPC    0x08000000
#define CLONE_NEWUSER   0x10000000
#define CLONE_NEWPID    0x20000000
#define CLONE_NEWNET    0x40000000

struct proc;

/* Refcount and lock shared by every namespace. */
typedef struct ns_common {
    uint32_t   refcount;
    spinlock_t lock;
} ns_common_t;

/* UTS Namespace */
typedef struct uts_namespace {
    char        nodename[65];
    char        domainname[65];
    ns_common_t ns;
} uts_namespace_t;

/* IPC Namespace */
typedef struct ipc_namespace {
    ns_common_t ns;
    void       *sysv_ids;
} ipc_namespace_t;

/* Mount Namespace */
typedef struct mnt_namespace {
    uint64_t    id;
    ns_common_t ns;
    void       *root_mount;
} mnt_namespace_t;

/* PID Namespace */
typedef struct pid_namespace {
    uint64_t              level;
    struct pid_namespace *parent;
    uint64_t              pid_max;
    uint64_t              next_pid;
    ns_common_t           ns;
    struct proc          *child_reaper;
    bool                  dead;
} pid_namespace_t;

/* Network Namespace */
typedef struct net_namespace {
    ns_common_t ns;
    void       *loopback_dev;
} net_namespace_t;

typedef struct uid_gid_extent {
    uint32_t first;
    uint32_t lower_first;
    uint32_t count;
} uid_gid_extent_t;

typedef struct user_namespace {
    struct user_namespace *parent;
    uint32_t               owner_uid;
    uint32_t               owner_gid;
    uint32_t               uid_extent_count;
    uid_gid_extent_t       uid_map[UID_GID_MAP_MAX];
    uint32_t               gid_extent_count;
    uid_gid_extent_t       gid_map[UID_GID_MAP_MAX];
    bool                   setgroups_allowed;
    ns_common_t            ns;
} user_namespace_t;

/* Cgroup Namespace */
typedef struct cgroup_namespace {
    void       *root_cgroup;
    ns_common_t ns;
} cgroup_namespace_t;

/* Namespace Proxy grouping all 7 namespaces */
typedef struct nsproxy {
    uts_namespace_t    *uts_ns;
    ipc_namespace_t    *ipc_ns;
    mnt_namespace_t    *mnt_ns;
    pid_namespace_t    *pid_ns;
    net_namespace_t    *net_ns;
    user_namespace_t   *user_ns;
    cgroup_namespace_t *cgroup_ns;
    ns_common_t         ns;
} nsproxy_t;

extern nsproxy_t          init_nsproxy;
extern uts_namespace_t    init_uts_ns;
extern ipc_namespace_t    init_ipc_ns;
extern mnt_namespace_t    init_mnt_ns;
extern pid_namespace_t    init_pid_ns;
extern net_namespace_t    init_net_ns;
extern user_namespace_t   init_user_ns;
extern cgroup_namespace_t init_cgroup_ns;

/* Initialize namespace subsystem */
void namespace_init(void);

/* Return the UTS namespace of the current process, or the initial one outside process context */
uts_namespace_t *uts_namespace_current(void);

/* Take/put a reference on an nsproxy */
nsproxy_t *nsproxy_get(nsproxy_t *ns);
void       nsproxy_put(nsproxy_t *ns);

/* Clone nsproxy based on clone_flags */
nsproxy_t *nsproxy_clone(nsproxy_t *orig, uint64_t flags, int *error);

/* Unshare specified namespaces for current task */
int namespace_unshare(uint64_t unshare_flags);

/* Switch namespace via fd */
int namespace_setns(int fd, int nstype);

/* Individual namespace helpers */
uts_namespace_t *uts_ns_get(uts_namespace_t *ns);
void             uts_ns_put(uts_namespace_t *ns);

ipc_namespace_t *ipc_ns_get(ipc_namespace_t *ns);
void             ipc_ns_put(ipc_namespace_t *ns);

mnt_namespace_t *mnt_ns_get(mnt_namespace_t *ns);
void             mnt_ns_put(mnt_namespace_t *ns);

pid_namespace_t *pid_ns_get(pid_namespace_t *ns);
void             pid_ns_put(pid_namespace_t *ns);

net_namespace_t *net_ns_get(net_namespace_t *ns);
void             net_ns_put(net_namespace_t *ns);

user_namespace_t *user_ns_get(user_namespace_t *ns);
void              user_ns_put(user_namespace_t *ns);

cgroup_namespace_t *cgroup_ns_get(cgroup_namespace_t *ns);
void                cgroup_ns_put(cgroup_namespace_t *ns);

#endif /* GNUCOS_NAMESPACE_H */
