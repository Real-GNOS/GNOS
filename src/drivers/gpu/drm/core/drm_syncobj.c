/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_syncobj.c - DRM_SYNCOBJ ioctls for /dev/dri. (GPLv2)
 *
 * GNOS has no asynchronous GPU work: the CPU renders everything in
 * software and the result is visible the instant the draw call returns.
 * A syncobj therefore models a fence that is already signalled the moment
 * it is created (or, for a timeline syncobj, the moment its point is
 * reached).  The ioctls still exist byte-for-byte as Linux defines them so
 * that an unmodified libdrm -- and Mesa's kopper / DRISWRAST GLX path that
 * drives buffer-presentation fencing through them -- keeps working.
 *
 * Even though the fence is conceptually always signalled, the handle table
 * and the sync_file file descriptors are real, so the lifetime and sharing
 * rules Mesa expects are honoured.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm.h"
#include "drm_device.h"
#include "drm_idr.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "syscall.h"
#include "vfs.h"
#include "anonfd.h"

struct drm_syncobj {
    uint32_t  handle;   /* local file handle (informational) */
    uint64_t  point;    /* timeline point reached; always "signalled" */
    uint32_t  refcount;
};

/* A sync_file file descriptor we hand out on export.  It represents an
 * already-signalled fence, so poll/read report readiness immediately. */
static int syncfile_node_poll(vfs_node_t *node, int16_t events, int16_t *revents)
{
    (void)node;

    /* No asynchronous work to wait for: always ready. */
    if (revents != NULL) {
        *revents = (int16_t)(events & (POLLIN | POLLOUT));
    }
    return 0;
}

static const vfs_ops_t g_syncfile_ops = {
    .poll = syncfile_node_poll,
};

/* Make an anonymous file descriptor that models a signalled sync_file. */
static int drm_syncobj_export_syncfile_fd(void)
{
    int h = vfs_anon_open(VFS_ANON, &g_syncfile_ops, NULL, O_RDWR);
    int fd;

    if (h < 0) {
        return h;
    }
    fd = anon_bind(h, 1 /* CLOEXEC */);
    if (fd < 0) {
        vfs_file_unref(h);
        return fd;
    }
    return fd;
}

static struct drm_syncobj *drm_syncobj_new(uint32_t handle)
{
    struct drm_syncobj *so = malloc(sizeof(*so));

    if (so == NULL) {
        return NULL;
    }
    memset(so, 0, sizeof(*so));
    so->handle   = handle;
    so->point    = 0;
    so->refcount = 1;
    return so;
}

static void drm_syncobj_get(struct drm_syncobj *so)
{
    if (so != NULL) {
        so->refcount++;
    }
}

static void drm_syncobj_put(struct drm_syncobj *so)
{
    if (so != NULL && --so->refcount == 0) {
        free(so);
    }
}

static struct drm_syncobj *drm_syncobj_lookup(struct drm_file *file, uint32_t handle)
{
    struct drm_syncobj *so;

    spin_lock(&file->syncobj_lock);
    so = (struct drm_syncobj *)drm_idr_find(&file->syncobj_idr, handle);
    if (so != NULL) {
        drm_syncobj_get(so);
    }
    spin_unlock(&file->syncobj_lock);
    return so;
}

static void drm_syncobj_unref_locked(struct drm_file *file, uint32_t handle)
{
    struct drm_syncobj *so = (struct drm_syncobj *)drm_idr_find(&file->syncobj_idr, handle);

    if (so != NULL) {
        drm_idr_remove(&file->syncobj_idr, handle);
        spin_unlock(&file->syncobj_lock);
        drm_syncobj_put(so);
        return;
    }
    spin_unlock(&file->syncobj_lock);
}

/*
 * DRM_IOCTL_SYNCOBJ_CREATE: allocate a syncobj and return its handle.
 */
int drm_syncobj_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_create *args = (struct drm_syncobj_create *)data;
    struct drm_syncobj       *so;
    uint32_t                 handle;
    int                      ret;

    (void)dev;

    if (file_priv == NULL || args == NULL) {
        return -EINVAL;
    }
    if ((args->flags & ~0x1u) != 0) {
        return -EINVAL;
    }

    so = drm_syncobj_new(0);
    if (so == NULL) {
        return -ENOMEM;
    }
    spin_lock(&file_priv->syncobj_lock);
    ret = drm_idr_alloc(&file_priv->syncobj_idr, so, 1, 0, &handle);
    if (ret != 0) {
        spin_unlock(&file_priv->syncobj_lock);
        free(so);
        return ret;
    }
    so->handle = handle;
    /* Signalled immediately: there is no asynchronous completion to wait
     * for, unless the caller explicitly asked for an unsignalled object. */
    if ((args->flags & DRM_SYNCOBJ_CREATE_SIGNALED) == 0) {
        so->point = 0; /* still "reached" -- software has no pending work */
    }
    spin_unlock(&file_priv->syncobj_lock);

    args->handle = handle;
    return 0;
}

/*
 * DRM_IOCTL_SYNCOBJ_DESTROY: drop a handle.
 */
int drm_syncobj_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_destroy *args = (struct drm_syncobj_destroy *)data;

    (void)dev;
    if (file_priv == NULL || args == NULL) {
        return -EINVAL;
    }
    drm_syncobj_unref_locked(file_priv, args->handle);
    return 0;
}

static int drm_syncobj_array_lookup(struct drm_file *file, const uint64_t uhandles, uint32_t count,
                                    struct drm_syncobj **out)
{
    uint32_t *handles;
    int       ret = 0;

    handles = malloc(count * sizeof(uint32_t));
    if (handles == NULL) {
        return -ENOMEM;
    }
    if (copy_from_user(handles, (const void *)(uintptr_t)uhandles, count * sizeof(uint32_t)) != 0) {
        free(handles);
        return -EFAULT;
    }
    for (uint32_t i = 0; i < count; i++) {
        out[i] = drm_syncobj_lookup(file, handles[i]);
        if (out[i] == NULL) {
            ret = -ENOENT;
            for (uint32_t j = 0; j < i; j++) {
                drm_syncobj_put(out[j]);
                out[j] = NULL;
            }
            break;
        }
    }
    free(handles);
    return ret;
}

/*
 * DRM_IOCTL_SYNCOBJ_RESET: mark the named syncobjs unsignalled.  On a real
 * GPU this arms them for later signalling; here it is a no-op on the fence
 * state (the fence is always considered signalled) but still drops the
 * references Mesa expects to be able to re-signal.
 */
int drm_syncobj_reset_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_reset  *args = (struct drm_syncobj_reset *)data;
    struct drm_syncobj      **so;
    int                       ret;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->count_handles == 0 || args->handles == 0) {
        return -EINVAL;
    }
    so = malloc(args->count_handles * sizeof(*so));
    if (so == NULL) {
        return -ENOMEM;
    }
    ret = drm_syncobj_array_lookup(file_priv, args->handles, args->count_handles, so);
    if (ret == 0) {
        /* Nothing to change: every software fence is already complete. */
    }
    for (uint32_t i = 0; i < args->count_handles; i++) {
        if (so[i] != NULL) {
            drm_syncobj_put(so[i]);
        }
    }
    free(so);
    return ret;
}

/*
 * DRM_IOCTL_SYNCOBJ_SIGNAL: mark the named syncobjs signalled.  Always true
 * here, so this just validates the handles.
 */
int drm_syncobj_signal_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_signal  *args = (struct drm_syncobj_signal *)data;
    struct drm_syncobj       **so;
    int                        ret;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->count_handles == 0 || args->handles == 0) {
        return -EINVAL;
    }
    so = malloc(args->count_handles * sizeof(*so));
    if (so == NULL) {
        return -ENOMEM;
    }
    ret = drm_syncobj_array_lookup(file_priv, args->handles, args->count_handles, so);
    for (uint32_t i = 0; i < args->count_handles; i++) {
        if (so[i] != NULL) {
            so[i]->point = UINT64_MAX; /* timeline fully reached */
            drm_syncobj_put(so[i]);
        }
    }
    free(so);
    return ret;
}

/*
 * DRM_IOCTL_SYNCOBJ_WAIT: wait for the named syncobjs to be signalled.
 * They always are, so this returns success (0) immediately, honouring the
 * timeout contract without blocking.
 */
int drm_syncobj_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_wait *args = (struct drm_syncobj_wait *)data;
    struct drm_syncobj    **so;
    int                    ret;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->count_handles == 0 || args->handles == 0) {
        return -EINVAL;
    }
    so = malloc(args->count_handles * sizeof(*so));
    if (so == NULL) {
        return -ENOMEM;
    }
    ret = drm_syncobj_array_lookup(file_priv, args->handles, args->count_handles, so);
    if (ret == 0) {
        args->first_signaled = 0; /* all already signalled */
    }
    for (uint32_t i = 0; i < args->count_handles; i++) {
        if (so[i] != NULL) {
            drm_syncobj_put(so[i]);
        }
    }
    free(so);
    return ret;
}

/*
 * DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: like WAIT but against a timeline point.
 * The point is always reached, so it succeeds immediately.
 */
int drm_syncobj_timeline_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_timeline_wait *args = (struct drm_syncobj_timeline_wait *)data;
    struct drm_syncobj             **so;
    uint64_t                        points[256];
    int                             ret;
    uint32_t                        n;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->count_handles == 0 || args->handles == 0) {
        return -EINVAL;
    }
    n = args->count_handles;
    if (n > 256) {
        n = 256;
    }
    if (copy_from_user(points, (const void *)(uintptr_t)args->points, n * sizeof(uint64_t)) != 0) {
        return -EFAULT;
    }
    so = malloc(args->count_handles * sizeof(*so));
    if (so == NULL) {
        return -ENOMEM;
    }
    ret = drm_syncobj_array_lookup(file_priv, args->handles, args->count_handles, so);
    if (ret == 0) {
        args->first_signaled = 0;
    }
    for (uint32_t i = 0; i < args->count_handles; i++) {
        if (so[i] != NULL) {
            drm_syncobj_put(so[i]);
        }
    }
    free(so);
    return ret;
}

/*
 * DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: advance the named timeline points.
 */
int drm_syncobj_timeline_signal_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_timeline_signal *args = (struct drm_syncobj_timeline_signal *)data;
    struct drm_syncobj              **so;
    uint64_t                         points[256];
    int                              ret;
    uint32_t                         n;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->count_handles == 0 || args->handles == 0) {
        return -EINVAL;
    }
    n = args->count_handles;
    if (n > 256) {
        n = 256;
    }
    if (copy_from_user(points, (const void *)(uintptr_t)args->points, n * sizeof(uint64_t)) != 0) {
        return -EFAULT;
    }
    so = malloc(args->count_handles * sizeof(*so));
    if (so == NULL) {
        return -ENOMEM;
    }
    ret = drm_syncobj_array_lookup(file_priv, args->handles, args->count_handles, so);
    for (uint32_t i = 0; i < args->count_handles; i++) {
        if (so[i] != NULL) {
            if (i < n) {
                so[i]->point = points[i];
            }
            drm_syncobj_put(so[i]);
        }
    }
    free(so);
    return ret;
}

/*
 * DRM_IOCTL_SYNCOBJ_QUERY: report the reached timeline point of each
 * syncobj.  Every software fence is complete, so report UINT64_MAX.
 */
int drm_syncobj_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_query *args = (struct drm_syncobj_query *)data;
    struct drm_syncobj     **so;
    uint64_t                 out[256];
    int                      ret;
    uint32_t                 n;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->count_handles == 0 || args->handles == 0) {
        return -EINVAL;
    }
    n = args->count_handles;
    if (n > 256) {
        n = 256;
    }
    so = malloc(args->count_handles * sizeof(*so));
    if (so == NULL) {
        return -ENOMEM;
    }
    ret = drm_syncobj_array_lookup(file_priv, args->handles, args->count_handles, so);
    if (ret == 0) {
        for (uint32_t i = 0; i < n; i++) {
            out[i] = (so[i] != NULL) ? UINT64_MAX : 0;
        }
        if (copy_to_user((void *)(uintptr_t)args->points, out, n * sizeof(uint64_t)) != 0) {
            ret = -EFAULT;
        }
    }
    for (uint32_t i = 0; i < args->count_handles; i++) {
        if (so[i] != NULL) {
            drm_syncobj_put(so[i]);
        }
    }
    free(so);
    return ret;
}

/*
 * DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: export a syncobj as a sync_file fd.
 */
int drm_syncobj_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_handle_to_fd *args = (struct drm_syncobj_handle_to_fd *)data;
    struct drm_syncobj             *so;
    int                            fd;

    (void)dev;
    if (file_priv == NULL || args == NULL) {
        return -EINVAL;
    }
    so = drm_syncobj_lookup(file_priv, args->handle);
    if (so == NULL) {
        return -ENOENT;
    }
    fd = drm_syncobj_export_syncfile_fd();
    drm_syncobj_put(so);
    if (fd < 0) {
        return fd;
    }
    args->fd = fd;
    return 0;
}

/*
 * DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: import a sync_file fd as a syncobj.
 */
int drm_syncobj_fd_to_handle_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_fd_to_handle *args = (struct drm_syncobj_fd_to_handle *)data;
    struct drm_syncobj             *so;
    uint32_t                        handle;
    int                             ret;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->fd < 0) {
        return -EINVAL;
    }
    /* The fd is a sync_file we (or any fence source) exported; the fence is
     * signalled, so the imported syncobj is complete. */
    so = drm_syncobj_new(0);
    if (so == NULL) {
        return -ENOMEM;
    }
    so->point = UINT64_MAX;
    spin_lock(&file_priv->syncobj_lock);
    ret = drm_idr_alloc(&file_priv->syncobj_idr, so, 1, 0, &handle);
    if (ret != 0) {
        spin_unlock(&file_priv->syncobj_lock);
        free(so);
        return ret;
    }
    so->handle = handle;
    spin_unlock(&file_priv->syncobj_lock);

    args->handle = handle;
    return 0;
}

/*
 * DRM_IOCTL_SYNCOBJ_EXPORT_SYNC_FILE: export the syncobj's fence as a
 * sync_file fd.
 */
int drm_syncobj_export_sync_file_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_export_sync_file *args = (struct drm_syncobj_export_sync_file *)data;
    struct drm_syncobj                *so;
    int                               fd;

    (void)dev;
    if (file_priv == NULL || args == NULL) {
        return -EINVAL;
    }
    so = drm_syncobj_lookup(file_priv, args->handle);
    if (so == NULL) {
        return -ENOENT;
    }
    fd = drm_syncobj_export_syncfile_fd();
    drm_syncobj_put(so);
    if (fd < 0) {
        return fd;
    }
    args->fd = fd;
    return 0;
}

/*
 * DRM_IOCTL_SYNCOBJ_IMPORT_SYNC_FILE: import a sync_file fd into the
 * syncobj, signalling it.
 */
int drm_syncobj_import_sync_file_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_import_sync_file *args = (struct drm_syncobj_import_sync_file *)data;
    struct drm_syncobj                *so;

    (void)dev;
    if (file_priv == NULL || args == NULL || args->fd < 0) {
        return -EINVAL;
    }
    so = drm_syncobj_lookup(file_priv, args->handle);
    if (so == NULL) {
        return -ENOENT;
    }
    so->point = UINT64_MAX; /* imported fence is already signalled */
    drm_syncobj_put(so);
    return 0;
}

/*
 * DRM_IOCTL_SYNCOBJ_TRANSFER: copy a timeline point from one syncobj to
 * another.
 */
int drm_syncobj_transfer_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_syncobj_transfer *args = (struct drm_syncobj_transfer *)data;
    struct drm_syncobj         *src, *dst;

    (void)dev;
    if (file_priv == NULL || args == NULL) {
        return -EINVAL;
    }
    src = drm_syncobj_lookup(file_priv, args->src_handle);
    dst = drm_syncobj_lookup(file_priv, args->dst_handle);
    if (src == NULL || dst == NULL) {
        if (src != NULL) {
            drm_syncobj_put(src);
        }
        if (dst != NULL) {
            drm_syncobj_put(dst);
        }
        return -ENOENT;
    }
    dst->point = args->dst_point;
    drm_syncobj_put(src);
    drm_syncobj_put(dst);
    return 0;
}
