/*
 * drm_ioctl.c - the door every DRM request comes through. (GPLv2)
 *
 * Every DRM command arrives as an ioctl number and a user pointer.  This
 * file is the single place that decides three things before any driver code
 * runs: is the number really a DRM command, how big is the argument, and is
 * this caller allowed to do that.  Only then is the argument copied into a
 * private kernel buffer and handed to a handler -- so a handler can treat it
 * as ordinary memory and cannot be handed a request that changes underneath
 * it.
 *
 * Commands come from three places: the driver's own table, the dumb-buffer
 * and PRIME fallbacks below, and the core table.  Unknown commands get
 * ENOTTY, except the lease ioctls, which get EOPNOTSUPP on purpose: clients
 * such as wlroots read that as "no leases here" and fall back, where ENOTTY
 * makes them give up on the device entirely.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm.h"
#include "drm_device.h"
#include "drm_mode.h"
#include "drm_port.h" /* copy_from_user / copy_to_user wrappers */
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "vfs.h"

/* ------------------------------------------------------- handlers elsewhere */

/* auth (drm_auth.c) */
extern int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_setmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_dropmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* GEM (drm_gem.c) */
extern int drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev, struct drm_mode_create_dumb *args);
extern int drm_gem_dumb_map_offset(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle, uint64_t *offset);
extern int drm_gem_dumb_destroy(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle);
extern int drm_gem_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv, uint32_t handle, uint32_t flags,
                                      int *prime_fd);
extern int drm_gem_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv, int prime_fd, uint32_t *handle);

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
extern int drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS cursor / page-flip / atomic (drm_atomic_uapi.c) */
extern int drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS vblank (drm_vblank.c) */
extern int drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS resources (drm_mode_config.c) */
extern int drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS properties (drm_property.c, drm_mode_object.c) */
extern int drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_getpropblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_createpropblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_destroypropblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_getgamma_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_setgamma_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_obj_getproperties_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_obj_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
extern int drm_mode_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* ------------------------------------------------------------------ permits */

int drm_ioctl_permit(unsigned int flags, struct drm_file *file_priv)
{
    if (file_priv == NULL) { return -EACCES; }

    if ((flags & DRM_AUTH) != 0 && !file_priv->authenticated) { return -EACCES; }

    if ((flags & DRM_MASTER) != 0) {
        /* There is no device-level master bookkeeping here, and the clients
         * that matter run under a seat launcher that never issues
         * SET_MASTER.  An authenticated client is the master. */
        if (!file_priv->authenticated) { return -EACCES; }
    }

    /* No notion of root in this kernel, so deny rather than guess. */
    if ((flags & DRM_ROOT_ONLY) != 0) { return -EACCES; }

    return 0;
}

/* ------------------------------------------------------------------ version */

/*
 * Copy @value into the caller's buffer, reporting its full length even when
 * the buffer is too small or absent -- that is how a client learns how much
 * to allocate.
 */
static int drm_version_copy_string(uint64_t user_ptr, uint64_t capacity, uint64_t *length, const char *value)
{
    size_t full_length;
    size_t copy_length;

    if (length == NULL) { return -EINVAL; }
    if (value == NULL) { value = ""; }

    full_length = strlen(value);
    *length     = full_length;

    if (user_ptr == 0 || capacity == 0) { return 0; }

    /* drmGetVersion() allocates the reported length plus room for a NUL. */
    copy_length = (capacity - 1 < full_length) ? (size_t)capacity - 1 : full_length;
    if (copy_length != 0 && copy_to_user((void *)(uintptr_t)user_ptr, value, copy_length) != 0) { return -EFAULT; }
    if (copy_to_user((void *)(uintptr_t)(user_ptr + copy_length), "\0", 1) != 0) { return -EFAULT; }

    return 0;
}

int drm_version(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_version *ver;
    uint64_t            name_ptr, name_capacity;
    uint64_t            date_ptr, date_capacity;
    uint64_t            desc_ptr, desc_capacity;
    int                 ret;

    (void)file_priv;

    if (dev == NULL || data == NULL) { return -EINVAL; }

    ver = (struct drm_version *)data;

    /* These fields carry a pointer in and a length out, so the pointers have
     * to be saved before the struct is filled in. */
    name_ptr      = ver->name;
    name_capacity = ver->name_len;
    date_ptr      = ver->date;
    date_capacity = ver->date_len;
    desc_ptr      = ver->desc;
    desc_capacity = ver->desc_len;

    memset(ver, 0, sizeof(*ver));
    ver->name = name_ptr;
    ver->date = date_ptr;
    ver->desc = desc_ptr;

    if (dev->driver == NULL) { return 0; }

    ver->version_major      = dev->driver->major;
    ver->version_minor      = dev->driver->minor;
    ver->version_patchlevel = dev->driver->patchlevel;

    ret = drm_version_copy_string(name_ptr, name_capacity, &ver->name_len, dev->driver->name);
    if (ret != 0) { return ret; }
    ret = drm_version_copy_string(date_ptr, date_capacity, &ver->date_len, dev->driver->date);
    if (ret != 0) { return ret; }
    ret = drm_version_copy_string(desc_ptr, desc_capacity, &ver->desc_len, dev->driver->desc);
    if (ret != 0) { return ret; }

    return 0;
}

/* DRM_IOCTL_SET_VERSION: there is one interface version here, so any request
 * that is not nonsense is accepted. */
int drm_setversion(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    (void)dev;
    (void)data;
    (void)file_priv;

    return 0;
}

/* --------------------------------------------------------------- capabilities */

int drm_get_cap(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_get_cap *cap = (struct drm_get_cap *)data;

    (void)file_priv;

    if (dev == NULL || cap == NULL) { return -EINVAL; }

    switch (cap->capability) {
        case DRM_CAP_DUMB_BUFFER:
            cap->value = 1;
            break;
        case DRM_CAP_VBLANK_HIGH_CRTC:
            cap->value = 1;
            break;
        case DRM_CAP_DUMB_PREFERRED_DEPTH:
            cap->value = 32;
            break;
        case DRM_CAP_DUMB_PREFER_SHADOW:
            cap->value = 0;
            break;
        case DRM_CAP_PRIME:
            cap->value = (dev->driver != NULL && (dev->driver->driver_features & DRIVER_PRIME) != 0)
                             ? (DRM_PRIME_CAP_EXPORT | DRM_PRIME_CAP_IMPORT)
                             : 0;
            break;
        case DRM_CAP_TIMESTAMP_MONOTONIC:
            cap->value = 1;
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
        case DRM_CAP_ADDFB2_MODIFIERS:
            cap->value = 0; /* one linear layout, no modifiers */
            break;
        case DRM_CAP_PAGE_FLIP_TARGET:
            cap->value = 0;
            break;
        case DRM_CAP_CRTC_IN_VBLANK_EVENT:
            cap->value = 1;
            break;
        case DRM_CAP_SYNCOBJ:
            cap->value = 0;
            break;
        case DRM_CAP_SYNCOBJ_TIMELINE:
            cap->value = 0;
            break;
        case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP:
            cap->value = 0;
            break;
        default:
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
            /* Only a driver that offers atomic can grant it, and asking for
             * it implies universal planes -- an atomic commit names every
             * plane, not just the primary and cursor. */
            if (cap->value > 1 || dev->driver == NULL || (dev->driver->driver_features & DRIVER_ATOMIC) == 0) {
                return -EINVAL;
            }
            file_priv->atomic = (cap->value != 0);
            if (cap->value != 0) { file_priv->universal_planes = true; }
            break;
        case DRM_CLIENT_CAP_ASPECT_RATIO:
            file_priv->aspect_ratio_allowed = (cap->value != 0);
            break;
        case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
            file_priv->writeback_connectors_allowed_unused = (cap->value != 0);
            break;
        case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT:
            break; /* accepted, nothing to record */
        default:
            return -EINVAL;
    }

    return 0;
}

/* ----------------------------------------------------------------- dispatch */

/*
 * MODE_CLOSEFB is the modern single-word twin of RMFB: same fb_id in, the
 * framebuffer goes away.  Clients prefer it and only fall back to RMFB on
 * EINVAL, so recognising it keeps teardown quiet.
 */
static int drm_mode_closefb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    return drm_mode_rmfb(dev, data, file_priv);
}

/* The commands every DRM device answers.  A NULL handler means "accepted,
 * does nothing", which is how the harmless legacy probes are handled. */
static const struct drm_ioctl_desc drm_core_ioctls[] = {
    /* 0x00 - 0x0d: core, GEM and capability queries */
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

    /* 0x2d - 0x2e: PRIME, reached through the fallback below */
    {DRM_IOCTL_PRIME_HANDLE_TO_FD,     NULL,                             DRM_AUTH             },
    {DRM_IOCTL_PRIME_FD_TO_HANDLE,     NULL,                             DRM_AUTH             },

    /* 0x3a: vblank */
    {DRM_IOCTL_WAIT_VBLANK,            drm_wait_vblank_ioctl,            0                    },

    /* 0xA0 - 0xBF: KMS */
    {DRM_IOCTL_MODE_GETRESOURCES,      drm_mode_getresources,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCRTC,           drm_mode_getcrtc,                 DRM_AUTH             },
    {DRM_IOCTL_MODE_SETCRTC,           drm_mode_setcrtc,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR,            drm_mode_cursor_ioctl,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETENCODER,        drm_mode_getencoder,              DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCONNECTOR,      drm_mode_getconnector,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPROPERTY,       drm_mode_getproperty_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPROPERTY,       drm_mode_setproperty_ioctl,       DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETPROPBLOB,       drm_mode_getpropblob_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_CREATEPROPBLOB,    drm_mode_createpropblob_ioctl,    DRM_AUTH             },
    {DRM_IOCTL_MODE_DESTROYPROPBLOB,   drm_mode_destroypropblob_ioctl,   DRM_AUTH             },
    {DRM_IOCTL_MODE_GETGAMMA,          drm_mode_getgamma_ioctl,          DRM_AUTH             },
    {DRM_IOCTL_MODE_SETGAMMA,          drm_mode_setgamma_ioctl,          DRM_MASTER | DRM_AUTH},
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

static const struct drm_ioctl_desc *find_ioctl_desc(unsigned int cmd, const struct drm_ioctl_desc *table, int count)
{
    for (int i = 0; i < count; i++) {
        if (table[i].cmd == cmd) { return &table[i]; }
    }
    return NULL;
}

/*
 * Dumb buffers and PRIME are dispatched here rather than from the table:
 * their handlers take the file and the device in the opposite order to
 * everything else, and PRIME has to be refused outright on a driver that
 * does not support it.
 *
 * Returns 0 with *@handled left alone when @cmd is not one of these.
 */
static int drm_ioctl_fallback(struct drm_device *dev, unsigned int cmd, void *kdata, struct drm_file *file_priv,
                              int *handled)
{
    int ret;

    if (cmd != DRM_IOCTL_MODE_CREATE_DUMB && cmd != DRM_IOCTL_MODE_MAP_DUMB && cmd != DRM_IOCTL_MODE_DESTROY_DUMB
        && cmd != DRM_IOCTL_PRIME_HANDLE_TO_FD && cmd != DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        return 0;
    }

    *handled = 1;

    /* All five need an authenticated client. */
    ret = drm_ioctl_permit(DRM_AUTH, file_priv);
    if (ret != 0) { return ret; }

    if (cmd == DRM_IOCTL_MODE_CREATE_DUMB) {
        return drm_gem_dumb_create(file_priv, dev, (struct drm_mode_create_dumb *)kdata);
    }

    if (cmd == DRM_IOCTL_MODE_MAP_DUMB) {
        struct drm_mode_map_dumb *args = (struct drm_mode_map_dumb *)kdata;

        return drm_gem_dumb_map_offset(file_priv, dev, args->handle, &args->offset);
    }

    if (cmd == DRM_IOCTL_MODE_DESTROY_DUMB) {
        struct drm_mode_destroy_dumb *args = (struct drm_mode_destroy_dumb *)kdata;

        return drm_gem_dumb_destroy(file_priv, dev, args->handle);
    }

    if (dev->driver == NULL || (dev->driver->driver_features & DRIVER_PRIME) == 0) { return -EOPNOTSUPP; }

    {
        struct drm_prime_handle *args = (struct drm_prime_handle *)kdata;

        if (cmd == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
            return drm_gem_prime_handle_to_fd(dev, file_priv, args->handle, args->flags, &args->fd);
        }
        return drm_gem_prime_fd_to_handle(dev, file_priv, args->fd, &args->handle);
    }
}

int drm_ioctl(struct drm_device *dev, unsigned int cmd, void *user_data, struct drm_file *file_priv)
{
    const struct drm_ioctl_desc *desc  = NULL;
    void                        *kdata = NULL;
    unsigned int                 dir;
    unsigned int                 size;
    int                          ret;

    if (dev == NULL || dev->driver == NULL || file_priv == NULL) {
        dbg_puts("DRMI: early EINVAL dev=");
        dbg_puts_hex((uint64_t)(uintptr_t)dev);
        dbg_puts(" drv=");
        dbg_puts_hex((dev != NULL) ? (uint64_t)(uintptr_t)dev->driver : 0);
        dbg_puts(" fp=");
        dbg_puts_hex((uint64_t)(uintptr_t)file_priv);
        dbg_puts("\r\n");
        return -EINVAL;
    }

    /* 1. Is it ours at all? */
    if (_IOC_TYPE(cmd) != DRM_IOCTL_BASE) { return -ENOTTY; }

    /* 2. Direction bits must be ones we understand. */
    dir = _IOC_DIR(cmd);
    if ((dir & ~(_IOC_READ | _IOC_WRITE)) != 0) { return -EINVAL; }

    /* 3. Cap the argument: no DRM command is anywhere near 16 KiB. */
    size = _IOC_SIZE(cmd);
    if (size > 0x4000) { return -EINVAL; }

    /* 4. Take a private copy of the argument.  Kernel and user share an
     * address space here, but the copy is what stops a handler writing
     * straight into memory the caller can also change. */
    if (size > 0) {
        kdata = malloc(size);
        if (kdata == NULL) { return -ENOMEM; }

        if ((dir & _IOC_WRITE) != 0) {
            if (copy_from_user(kdata, user_data, size) != 0) {
                free(kdata);
                return -EFAULT;
            }
        } else {
            memset(kdata, 0, size);
        }
    }

    /* 5. The driver's own commands come first. */
    if (dev->driver->ioctls != NULL && dev->driver->num_ioctls > 0) {
        desc = find_ioctl_desc(cmd, dev->driver->ioctls, dev->driver->num_ioctls);
    }

    /* 6. Then the dumb-buffer and PRIME fallbacks. */
    if (desc == NULL) {
        int handled = 0;

        ret = drm_ioctl_fallback(dev, cmd, kdata, file_priv, &handled);
        if (handled) { goto copy_out; }
    }

    /* 7. Then the core table. */
    if (desc == NULL) {
        desc = find_ioctl_desc(cmd, drm_core_ioctls, (int)(sizeof(drm_core_ioctls) / sizeof(drm_core_ioctls[0])));
    }

    if (desc == NULL) {
        /* Leases: say "not supported" rather than "not a command", so the
         * client falls back to opening the plain node instead of giving up. */
        if (cmd == DRM_IOCTL_MODE_CREATE_LEASE || cmd == DRM_IOCTL_MODE_LIST_LESSEES || cmd == DRM_IOCTL_MODE_GET_LEASE
            || cmd == DRM_IOCTL_MODE_REVOKE_LEASE) {
            ret = -EOPNOTSUPP;
            goto out;
        }
        ret = -ENOTTY;
        goto out;
    }

    /* 8. Permission check, then dispatch. */
    ret = drm_ioctl_permit(desc->flags, file_priv);
    if (ret != 0) {
        plogk("drm: ioctl 0x%x flags 0x%x DENIED: master=%d auth=%d\n", cmd, desc->flags, file_priv->master != NULL,
              file_priv->authenticated);
        goto out;
    }

    if (desc->func == NULL) {
        ret = 0; /* accepted, nothing to do */
        goto out;
    }

    ret = desc->func(dev, kdata, file_priv);

copy_out:
    /* 9. Hand back whatever the command produced. */
    if (ret == 0 && kdata != NULL && (dir & _IOC_READ) != 0 && copy_to_user(user_data, kdata, size) != 0) {
        ret = -EFAULT;
    }

out:
    dbg_puts("DRMI: cmd=0x");
    dbg_puts_hex(cmd);
    dbg_puts(" ret=");
    dbg_puts_dec((uint32_t)ret);
    dbg_puts(" auth=");
    dbg_puts_dec((file_priv != NULL) ? file_priv->authenticated : 9);
    dbg_puts("\r\n");

    free(kdata);
    return ret;
}
