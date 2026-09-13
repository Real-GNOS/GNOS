/*
 * drm_ioctl.c — ioctl dispatch for the GNOS DRM core.
 *
 * (GPLv2) This is a from-scratch implementation of the DRM ioctl layer for
 * the GNOS kernel.  It validates the ioctl encoding, copies the argument
 * into a private kernel buffer, checks per-command permissions, and hands
 * the command to the driver's table, the dumb-buffer/PRIME fallbacks, or
 * the core table below.
 *
 * Copyright 2026 GNOS contributors.
 */

#include "drm_port.h"   /* copy_from_user/copy_to_user wrappers */
#include "drm.h"
#include "drm_device.h"
#include "drm_mode.h"
#include "drm_print.h"
#include "vfs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kstring.h"
#include "heap.h"

/* ioctl handlers live in their respective subsystem files. */

/* auth (drm_auth.c) */
extern int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_setmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_dropmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* GEM (drm_gem.c) */
extern int drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS CRTC (drm_crtc.c) */
extern int drm_mode_getcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS encoder (drm_encoder.c) */
extern int drm_mode_getencoder(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS connector (drm_connector.c) */
extern int drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS plane (drm_plane.c) */
extern int drm_mode_getplane_res(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_getplane(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS framebuffer (drm_framebuffer.c) */
extern int drm_mode_addfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_addfb2(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_rmfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_getfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_dirtyfb(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* MODE_CLOSEFB is RMFB's one-word twin: feed it straight through.  wlroots
 * reaches for it first and treats ENOTTY as "too old", so answering with
 * the RMFB path keeps the compositor's teardown quiet. */
static int drm_mode_closefb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    return drm_mode_rmfb(dev, data, file_priv);
}

/* KMS cursor / page-flip / atomic (drm_atomic_uapi.c) */
extern int drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS vblank (drm_vblank.c) */
extern int drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS resources (drm_mode_config.c) */
extern int drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS property (drm_property.c) */
extern int drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_getpropblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_obj_getproperties_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_obj_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS getfb2 (drm_framebuffer.c) */
extern int drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* ------------------------------------------------------------ *
 * permission gate                                              *
 *                                                              *
 * Two bits matter here: DRM_AUTH demands the client already    *
 * hold a magic the kernel signed, and DRM_MASTER would demand  *
 * the master role.  There is no real master bookkeeping in     *
 * this kernel and no user of SET_MASTER, so every authenticated*
 * client simply acts as the master.  ROOT_ONLY has no meaning  *
 * without a root account, and we stay conservative: deny.      *
 * ------------------------------------------------------------ */
int drm_ioctl_permit(unsigned int flags, struct drm_file *file_priv)
{
    if (file_priv == NULL) { return -EACCES; }

    if ((flags & DRM_AUTH) != 0 && !file_priv->authenticated) { return -EACCES; }

    if ((flags & DRM_MASTER) != 0 && !file_priv->authenticated) { return -EACCES; }

    if ((flags & DRM_ROOT_ONLY) != 0) { return -EACCES; }

    return 0;
}

/* ------------------------------------------------------------ *
 * client capability negotiation                                *
 * ------------------------------------------------------------ */
int drm_get_cap(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_get_cap *cap = (struct drm_get_cap *)data;

    (void)file_priv;

    if (dev == NULL || cap == NULL) { return -EINVAL; }

    switch (cap->capability) {
        case DRM_CAP_DUMB_BUFFER:
        case DRM_CAP_VBLANK_HIGH_CRTC:
        case DRM_CAP_TIMESTAMP_MONOTONIC:
        case DRM_CAP_CRTC_IN_VBLANK_EVENT:
            cap->value = 1;
            break;

        case DRM_CAP_DUMB_PREFERRED_DEPTH:
            cap->value = 32;
            break;

        case DRM_CAP_DUMB_PREFER_SHADOW:
        case DRM_CAP_ADDFB2_MODIFIERS:
        case DRM_CAP_PAGE_FLIP_TARGET:
        case DRM_CAP_SYNCOBJ:
        case DRM_CAP_SYNCOBJ_TIMELINE:
        case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP:
            cap->value = 0;
            break;

        case DRM_CAP_PRIME:
            cap->value = (dev->driver->driver_features & DRIVER_PRIME)
                             ? (DRM_PRIME_CAP_EXPORT | DRM_PRIME_CAP_IMPORT)
                             : 0;
            break;

        case DRM_CAP_ASYNC_PAGE_FLIP:
            cap->value = dev->mode_config.async_page_flip ? 1 : 0;
            break;

        case DRM_CAP_CURSOR_WIDTH:
            cap->value = dev->mode_config.cursor_width;
            break;

        case DRM_CAP_CURSOR_HEIGHT:
            cap->value = dev->mode_config.cursor_height;
            break;

        default:
            /* unknown caps answer 0, they never fail */
            cap->value = 0;
            break;
    }

    return 0;
}

int drm_set_client_cap(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_set_client_cap *cap = (struct drm_set_client_cap *)data;

    if (data == NULL || file_priv == NULL) { return -EINVAL; }

    switch (cap->capability) {
        case DRM_CLIENT_CAP_STEREO_3D:
            file_priv->stereo3d_allowed_unused = (cap->value != 0);
            break;

        case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
            if (cap->value > 1) { return -EINVAL; }
            file_priv->universal_planes = (cap->value != 0);
            break;

        case DRM_CLIENT_CAP_ATOMIC:
            /* atomic is a promise the whole stack must keep: only offer it
             * when the driver really drives atomic modesets */
            if (cap->value > 1 || (dev->driver->driver_features & DRIVER_ATOMIC) == 0) { return -EINVAL; }
            file_priv->atomic = (cap->value != 0);
            if (cap->value) { file_priv->universal_planes = true; }
            break;

        case DRM_CLIENT_CAP_ASPECT_RATIO:
            file_priv->aspect_ratio_allowed = (cap->value != 0);
            break;

        case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
            file_priv->writeback_connectors_allowed_unused = (cap->value != 0);
            break;

        case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT:
            break;

        default:
            return -EINVAL;
    }

    return 0;
}

/* ------------------------------------------------------------ *
 * the core command table                                       *
 *                                                              *
 * Driver-provided tables are searched first, then the dumb /   *
 * PRIME fallbacks, then this table.  An entry with a NULL func *
 * is a deliberate no-op that reports success (GET_UNIQUE).     *
 * ------------------------------------------------------------ */
static const struct drm_ioctl_desc drm_core_ioctls[] = {
    {DRM_IOCTL_VERSION,                drm_version,                      0                    },
    {DRM_IOCTL_GET_UNIQUE,             NULL,                             0                    },
    {DRM_IOCTL_GET_MAGIC,              drm_getmagic,                     DRM_AUTH             },
    {DRM_IOCTL_SET_VERSION,            drm_setversion,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_SET_MASTER,             drm_setmaster,                    0                    },
    {DRM_IOCTL_DROP_MASTER,            drm_dropmaster,                   0                    },
    {DRM_IOCTL_AUTH_MAGIC,             drm_authmagic,                    DRM_AUTH             },
    {DRM_IOCTL_GEM_CLOSE,              drm_gem_close_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_FLINK,              drm_gem_flink_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_OPEN,               drm_gem_open_ioctl,               DRM_AUTH             },
    {DRM_IOCTL_GET_CAP,                drm_get_cap,                      0                    },
    {DRM_IOCTL_SET_CLIENT_CAP,         drm_set_client_cap,               0                    },

    {DRM_IOCTL_PRIME_HANDLE_TO_FD,     NULL,                             DRM_AUTH             },
    {DRM_IOCTL_PRIME_FD_TO_HANDLE,     NULL,                             DRM_AUTH             },

    {DRM_IOCTL_WAIT_VBLANK,            drm_wait_vblank_ioctl,            0                    },

    {DRM_IOCTL_MODE_GETRESOURCES,      drm_mode_getresources,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCRTC,           drm_mode_getcrtc,                 DRM_AUTH             },
    {DRM_IOCTL_MODE_SETCRTC,           drm_mode_setcrtc,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR,            drm_mode_cursor_ioctl,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETENCODER,        drm_mode_getencoder,              DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCONNECTOR,      drm_mode_getconnector,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPROPERTY,       drm_mode_getproperty_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPROPERTY,       drm_mode_setproperty_ioctl,       DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETPROPBLOB,       drm_mode_getpropblob_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_GETFB,             drm_mode_getfb,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB,             drm_mode_addfb,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_RMFB,              drm_mode_rmfb,                    DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CLOSEFB,           drm_mode_closefb,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_PAGE_FLIP,         drm_mode_page_flip_ioctl,         DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_DIRTYFB,           drm_mode_dirtyfb,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CREATE_DUMB,       NULL,                             DRM_AUTH             },
    {DRM_IOCTL_MODE_MAP_DUMB,          NULL,                             DRM_AUTH             },
    {DRM_IOCTL_MODE_DESTROY_DUMB,      NULL,                             DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPLANE,          drm_mode_getplane,                DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPLANE,          drm_mode_setplane,                DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB2,            drm_mode_addfb2,                  DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_mode_obj_getproperties_ioctl, DRM_AUTH             },
    {DRM_IOCTL_MODE_OBJ_SETPROPERTY,   drm_mode_obj_setproperty_ioctl,   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR2,           drm_mode_cursor2_ioctl,           DRM_AUTH             },
    {DRM_IOCTL_MODE_ATOMIC,            drm_mode_atomic_ioctl,            DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB2,            drm_mode_getfb2_ioctl,            DRM_MASTER | DRM_AUTH},
};

#define DRM_CORE_IOCTL_COUNT (sizeof(drm_core_ioctls) / sizeof(drm_core_ioctls[0]))

static const struct drm_ioctl_desc *find_ioctl_desc(unsigned int cmd,
                                                    const struct drm_ioctl_desc *table,
                                                    int count)
{
    for (int i = 0; i < count; i++) {
        if (table[i].cmd == cmd) { return &table[i]; }
    }
    return NULL;
}

/* Handlers the driver table and the core table both leave NULL for; the
 * core supplies its own dumb-buffer and PRIME plumbing here. */
extern int drm_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev,
                               struct drm_mode_create_dumb *args);
extern int drm_gem_dumb_map_offset(struct drm_file *file_priv, struct drm_device *dev,
                                   uint32_t handle, uint64_t *offset);
extern int drm_gem_dumb_destroy(struct drm_file *file_priv, struct drm_device *dev,
                                uint32_t handle);
extern int drm_gem_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv,
                                      uint32_t handle, uint32_t flags, int *prime_fd);
extern int drm_gem_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
                                      int prime_fd, uint32_t *handle);

/* GET_UNIQUE: there is one DRM device and its bus id is fixed at boot, so
 * the answer never changes; copy it into the caller's buffer if it fits. */
static int drm_getunique_stub(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_unique *u = (struct drm_unique *)data;
    const char         *busid;
    size_t              len;

    (void)file_priv;

    if (u == NULL) { return -EINVAL; }

    busid = (dev->unique != NULL) ? dev->unique : "gnos-drm";
    len   = strlen(busid) + 1;

    if (u->unique_len < len) {
        u->unique_len = (uint64_t)len;
        return -EINVAL; /* caller retries with a bigger buffer */
    }

    if (u->unique == 0 || copy_to_user((void *)(uintptr_t)u->unique, busid, len) != 0) {
        return -EFAULT;
    }

    u->unique_len = (uint64_t)len;
    return 0;
}

int drm_version(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_version *v        = (struct drm_version *)data;
    static const char   name[]   = "gnos-drm";
    static const char   date[]   = "20260722";
    static const char   desc[]   = "GNOS DRM core";
    int                 rc;

    (void)file_priv;

    if (v == NULL) { return -EINVAL; }

    rc = 0;
    if (v->name_len >= sizeof(name)) {
        if (copy_to_user((void *)(uintptr_t)v->name, name, sizeof(name)) != 0) { return -EFAULT; }
        v->name_len = sizeof(name) - 1;
    } else {
        v->name_len = sizeof(name) - 1;
        rc          = -EINVAL;
    }
    if (v->date_len >= sizeof(date)) {
        if (copy_to_user((void *)(uintptr_t)v->date, date, sizeof(date)) != 0) { return -EFAULT; }
        v->date_len = sizeof(date) - 1;
    } else {
        v->date_len = sizeof(date) - 1;
        rc          = -EINVAL;
    }
    if (v->desc_len >= sizeof(desc)) {
        if (copy_to_user((void *)(uintptr_t)v->desc, desc, sizeof(desc)) != 0) { return -EFAULT; }
        v->desc_len = sizeof(desc) - 1;
    } else {
        v->desc_len = sizeof(desc) - 1;
        rc          = -EINVAL;
    }

    v->version_major      = 1;
    v->version_minor      = 0;
    v->version_patchlevel = 0;

    (void)dev;
    return rc;
}

/* SET_VERSION: only the interface major the client wants matters.  Bumping
 * to a major above ours would mean APIs we never had. */
int drm_setversion(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_set_version *sv = (struct drm_set_version *)data;

    (void)file_priv;

    if (sv == NULL) { return -EINVAL; }

    if (sv->drm_di_major > 1 || sv->drm_di_minor > 4) { return -EINVAL; }
    if (sv->drm_dd_major > 1) { return -EINVAL; }

    sv->drm_di_major = 1;
    sv->drm_di_minor = 4;
    sv->drm_dd_major = 1;
    sv->drm_dd_minor = 0;

    (void)dev;
    return 0;
}

static int drm_mode_create_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_create_dumb *args = (struct drm_mode_create_dumb *)data;
    int                          rc;

    if (args == NULL || file_priv == NULL) { return -EINVAL; }

    rc = drm_gem_dumb_create(dev, file_priv, args);
    if (rc == 0 && copy_to_user(data, args, sizeof(*args)) != 0) { return -EFAULT; }
    return rc;
}

static int drm_mode_map_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_map_dumb *args = (struct drm_mode_map_dumb *)data;
    uint64_t                  offset = 0;
    int                       rc;

    if (args == NULL || file_priv == NULL) { return -EINVAL; }

    rc = drm_gem_dumb_map_offset(file_priv, dev, args->handle, &offset);
    if (rc != 0) { return rc; }

    args->offset = offset;

    if (copy_to_user(data, args, sizeof(*args)) != 0) { return -EFAULT; }
    return 0;
}

static int drm_mode_destroy_dumb_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_destroy_dumb *args = (struct drm_mode_destroy_dumb *)data;

    if (args == NULL || file_priv == NULL) { return -EINVAL; }

    return drm_gem_dumb_destroy(file_priv, dev, args->handle);
}

static int drm_gem_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_prime_handle *args = (struct drm_prime_handle *)data;
    int                      fd   = -1;
    int                      rc;

    if (args == NULL || file_priv == NULL) { return -EINVAL; }

    rc = drm_gem_prime_handle_to_fd(dev, file_priv, args->handle, args->flags, &fd);
    if (rc != 0) { return rc; }

    args->fd = (uint32_t)fd;
    return 0;
}

static int drm_gem_prime_fd_to_handle_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_prime_handle *args   = (struct drm_prime_handle *)data;
    uint32_t                 handle = 0;
    int                      rc;

    if (args == NULL || file_priv == NULL) { return -EINVAL; }

    rc = drm_gem_prime_fd_to_handle(dev, file_priv, (int)args->fd, &handle);
    if (rc != 0) { return rc; }

    args->handle = handle;
    return 0;
}

static const struct drm_ioctl_desc *drm_find_command(struct drm_device *dev, unsigned int cmd)
{
    const struct drm_ioctl_desc *desc;

    /* 1. the driver's own table */
    if (dev->driver->ioctls != NULL && dev->driver->num_ioctls > 0) {
        desc = find_ioctl_desc(cmd, dev->driver->ioctls, dev->driver->num_ioctls);
        if (desc != NULL) { return desc; }
    }

    /* 2. core fallbacks for the entries the tables leave open */
    switch (cmd) {
        case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
            static const struct drm_ioctl_desc d = {DRM_IOCTL_PRIME_HANDLE_TO_FD,
                                                    drm_gem_prime_handle_to_fd_ioctl,
                                                    DRM_AUTH};
            return &d;
        }
        case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
            static const struct drm_ioctl_desc d = {DRM_IOCTL_PRIME_FD_TO_HANDLE,
                                                    drm_gem_prime_fd_to_handle_ioctl,
                                                    DRM_AUTH};
            return &d;
        }
        case DRM_IOCTL_MODE_CREATE_DUMB: {
            static const struct drm_ioctl_desc d = {DRM_IOCTL_MODE_CREATE_DUMB,
                                                    drm_mode_create_dumb_ioctl,
                                                    DRM_AUTH};
            return &d;
        }
        case DRM_IOCTL_MODE_MAP_DUMB: {
            static const struct drm_ioctl_desc d = {DRM_IOCTL_MODE_MAP_DUMB,
                                                    drm_mode_map_dumb_ioctl,
                                                    DRM_AUTH};
            return &d;
        }
        case DRM_IOCTL_MODE_DESTROY_DUMB: {
            static const struct drm_ioctl_desc d = {DRM_IOCTL_MODE_DESTROY_DUMB,
                                                    drm_mode_destroy_dumb_ioctl,
                                                    DRM_AUTH};
            return &d;
        }
        case DRM_IOCTL_GET_UNIQUE: {
            static const struct drm_ioctl_desc d = {DRM_IOCTL_GET_UNIQUE, drm_getunique_stub, 0};
            return &d;
        }
        default:
            break;
    }

    /* 3. the core table */
    desc = find_ioctl_desc(cmd, drm_core_ioctls, (int)DRM_CORE_IOCTL_COUNT);
    if (desc != NULL) { return desc; }

    return NULL;
}

/*
 * The entry point the char-device layer calls.  Steps, in order:
 *   1. decode the ioctl number (magic, type, size),
 *   2. copy the user argument into a kernel buffer of the declared size,
 *   3. look the command up and check its permission bits,
 *   4. run it,
 *   5. copy the (possibly rewritten) buffer back.
 */
int drm_ioctl(struct drm_device *dev, unsigned int cmd, void *data, struct drm_file *file_priv)
{
    const struct drm_ioctl_desc *desc;
    unsigned int                 size;
    char                         kdata[128];
    int                          ret;

    if (dev == NULL || file_priv == NULL) { return -EINVAL; }

    if (_IOC_TYPE(cmd) != DRM_IOCTL_BASE) { return -ENOTTY; }

    size = _IOC_SIZE(cmd);

    if (size == 0 || size > sizeof(kdata)) { return -EINVAL; }

    desc = drm_find_command(dev, cmd);
    if (desc == NULL) {
        DRM_ERROR("unsupported ioctl %08x (nr=%u size=%u)\n", cmd, _IOC_NR(cmd), size);
        return -ENOTTY;
    }

    ret = drm_ioctl_permit(desc->flags, file_priv);
    if (ret != 0) {
        DRM_ERROR("permission denied for ioctl %08x\n", cmd);
        return ret;
    }

    if (data != NULL && copy_from_user(kdata, data, size) != 0) { return -EFAULT; }

    if (desc->func == NULL) { return -EINVAL; }

    ret = desc->func(dev, kdata, file_priv);

    if (ret == 0 && data != NULL && _IOC_DIR(cmd) != 0 &&
        copy_to_user(data, kdata, size) != 0) {
        return -EFAULT;
    }

    return ret;
}
