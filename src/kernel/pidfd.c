/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pidfd.c — pidfd_open(434) / pidfd_send_signal(424) / pidfd_getfd(438).
 * (GPLv2)
 *
 * The descriptor is an anonymous VFS node whose private data is the pid,
 * which is enough to make the reference stable: the node keeps the process
 * slot alive for as long as the descriptor exists, so signalling through it
 * can never reach a recycled pid.
 */
#include <stdint.h>

#include "pidfd.h"
#include "proc.h"
#include "signal.h"
#include "anonfd.h"
#include "vfs.h"
#include "heap.h"
#include "kstring.h"

#define PIDFD_NONBLOCK 0x8000          /* O_NONBLOCK, as passed by musl */

typedef struct {
    int pid;
} pidfd_t;

static int32_t pidfd_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return 0;                            /* a pidfd is not readable */
}

static int32_t pidfd_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_INVAL;
}

static int32_t pidfd_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    (void)n; (void)cmd; (void)arg;
    return -E_NOTTY;
}

static void pidfd_release(vfs_node_t *n)
{
    if (n && n->priv)
        kfree(n->priv);
}

static const vfs_ops_t g_pidfd_ops = {
    .read    = pidfd_read,
    .write   = pidfd_write,
    .ioctl   = pidfd_ioctl,
    .release = pidfd_release,
};

/* Resolve a pidfd descriptor to the process it names. */
static proc_t *pidfd_proc(int fd)
{
    int h = fd_handle(fd);
    if (h < 0)
        return NULL;
    const vfs_node_t *n = vfs_file_node(h);
    if (!n || n->ops != &g_pidfd_ops || !n->priv)
        return NULL;
    return proc_by_pid(((pidfd_t *)n->priv)->pid);
}

int64_t sys_pidfd_open(uint64_t pid, uint64_t flags)
{
    if (flags & ~PIDFD_NONBLOCK)
        return -E_INVAL;
    if ((int)pid <= 0)
        return -E_INVAL;
    proc_t *target = proc_by_pid((int)pid);
    if (!target)
        return -E_SRCH;

    pidfd_t *pf = kmalloc(sizeof(pidfd_t));
    if (!pf)
        return -E_NOMEM;
    pf->pid = (int)pid;

    int h = vfs_anon_open(VFS_ANON, &g_pidfd_ops, pf, O_RDONLY);
    if (h < 0) {
        kfree(pf);
        return h;
    }
    return anon_bind(h, 0);
}

int64_t sys_pidfd_send_signal(uint64_t pidfd, uint64_t sig, uint64_t uinfo,
                              uint64_t flags)
{
    (void)uinfo;                          /* siginfo delivery not supported */
    if (flags)
        return -E_INVAL;
    proc_t *target = pidfd_proc((int)pidfd);
    if (!target)
        return -E_BADF;
    return proc_signal(target, (int)sig) == 0 ? 0 : -E_INVAL;
}

/* Not implemented: handing a descriptor from one process to another needs
 * the file table to be shared across a pidfd, which GNOS does not model. */
int64_t sys_pidfd_getfd(uint64_t pidfd, uint64_t targetfd, uint64_t flags)
{
    (void)pidfd; (void)targetfd; (void)flags;
    return -E_NOSYS;
}
