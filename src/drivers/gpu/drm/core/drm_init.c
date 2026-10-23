/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_init.c - DRM subsystem bootstrap and the global device registry.
 * (GPLv2)
 *
 * The registry is how the rest of the kernel asks for "the" display device
 * without knowing which driver bound to it.  The class is registered before
 * any driver probes, so a hardware driver can claim card0; only when none
 * did does the software device (simpledrm) take over.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_devtmpfs.h"
#include "drm_init.h"
#include "drm_print.h"
#include "simpledrm.h"
#include "smp.h"

/* ------------------------------------------------------- device registry */

/* Every registered device, in registration order.  Most of the kernel asks
 * for "the" device, which means the first one. */
#define DRM_MAX_DEVICES 16

static struct drm_device *drm_device_list[DRM_MAX_DEVICES];
static spinlock_t         drm_device_list_lock = {0};

void drm_device_list_add(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i] == NULL) {
            drm_device_list[i] = dev;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

void drm_device_list_remove(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i] == dev) {
            drm_device_list[i] = NULL;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

struct drm_device *drm_get_singleton(void)
{
    spin_lock(&drm_device_list_lock);

    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];

        if (dev != NULL) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
    }

    spin_unlock(&drm_device_list_lock);
    return NULL;
}

/* The device behind a given /dev/dri node, which is how a driver finds the
 * device an ioctl arrived on. */
struct drm_device *drm_get_device_by_minor(int type, int index)
{
    spin_lock(&drm_device_list_lock);

    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];

        if (dev == NULL) {
            continue;
        }

        if (type == DRM_MINOR_PRIMARY && dev->primary != NULL && dev->primary->index == index) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
        if (type == DRM_MINOR_RENDER && dev->render != NULL && dev->render->index == index) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
    }

    spin_unlock(&drm_device_list_lock);
    return NULL;
}

/* -------------------------------------------------------------- DRM class */

/* udev finds the device nodes through /sys/class/drm and expects this
 * uevent variable to tell it what kind of node it is looking at. */
static int drm_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    (void)dev;

    return add_uevent_var(env, "DEVTYPE=drm_minor");
}

struct class drm_class = {
    .name       = "drm",
    .dev_uevent = drm_device_uevent,
};
int drm_class_registered = 0;

/* ------------------------------------------------------------------- init */

#if CONFIG_DRM

int drm_init(void)
{
    /* Only the class: the software device must not claim card0 or renderD128
     * before a hardware driver has had its chance to probe. */
    if (!drm_class_registered) {
        int ret = class_register(&drm_class);

        if (ret != EOK) {
            return ret;
        }
        drm_class_registered = 1;
    }

    return 0;
}

/* The fallback device is the software display driver.  Keep this entry point
 * because the boot sequence knows only about the DRM class, not about which
 * driver ends up claiming card0. */
int drm_init_fallback(void)
{
    return simpledrm_init();
}

#else

int drm_init(void)
{
    return -ENODEV;
}

int drm_init_fallback(void)
{
    return -ENODEV;
}

#endif /* CONFIG_DRM */
