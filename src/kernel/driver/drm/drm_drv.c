/*
 * drm_drv.c - the life of a DRM device and of an open file on it. (GPLv2)
 *
 * A device is a driver plus a pile of KMS objects plus, once registered, the
 * /dev/dri nodes user space finds it through: one "primary" node that can do
 * everything, and a "render" node that can only allocate and render -- which
 * is how a compositor hands a client the ability to draw without also
 * handing it the ability to change the display.
 *
 * The other half of this file is the per-open bookkeeping: what happens when
 * somebody opens one of those nodes, and the precise order in which
 * everything they owned is given back when they close it.  That order is not
 * decorative -- framebuffers hold references to buffers, so they have to go
 * before the handles that name those buffers.
 */

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_devtmpfs.h" /* devtmpfs shim -> GNOS VFS */
#include "drm_hashtab.h"
#include "drm_init.h"
#include "drm_print.h"
#include "heap.h"
#include "intrusive_list.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

/* The DRM class, registered once by drm_init(). */
extern struct class drm_class;
extern int          drm_class_registered;

/* ------------------------------------------------------------ minor numbers */

/* One bitmap per node type: which indices are taken. */
static uint64_t   drm_minor_bitmap_primary;
static uint64_t   drm_minor_bitmap_render;
static uint64_t   drm_minor_bitmap_accel;
static spinlock_t drm_minor_lock = {0};

int drm_minor_alloc(int type)
{
    uint64_t *bitmap;

    switch (type) {
        case DRM_MINOR_PRIMARY: bitmap = &drm_minor_bitmap_primary; break;
        case DRM_MINOR_RENDER:  bitmap = &drm_minor_bitmap_render;  break;
        case DRM_MINOR_ACCEL:   bitmap = &drm_minor_bitmap_accel;   break;
        default: return -EINVAL;
    }

    spin_lock(&drm_minor_lock);
    for (int i = 0; i < DRM_MAX_MINOR; i++) {
        if ((*bitmap & (1ULL << i)) == 0) {
            *bitmap |= (1ULL << i);
            spin_unlock(&drm_minor_lock);
            return i;
        }
    }
    spin_unlock(&drm_minor_lock);

    return -ENOSPC;
}

void drm_minor_free(int type, int index)
{
    uint64_t *bitmap;

    if (index < 0 || index >= DRM_MAX_MINOR) { return; }

    switch (type) {
        case DRM_MINOR_PRIMARY: bitmap = &drm_minor_bitmap_primary; break;
        case DRM_MINOR_RENDER:  bitmap = &drm_minor_bitmap_render;  break;
        case DRM_MINOR_ACCEL:   bitmap = &drm_minor_bitmap_accel;   break;
        default: return;
    }

    spin_lock(&drm_minor_lock);
    *bitmap &= ~(1ULL << index);
    spin_unlock(&drm_minor_lock);
}

/* ------------------------------------------------------------ device set-up */

struct drm_master {
    struct drm_device   *dev;
    spinlock_t           lock;
    int                  unique_len;
    char                *unique;
    struct drm_open_hash magiclist;
    ilist_node_t         magicfree;
    int                  refcount;
};

/* Implemented in drm_file.c. */
struct drm_file *drm_file_alloc(struct drm_device *dev);
void             drm_file_free(struct drm_file *file);

/* Fill in one minor at an index already reserved by drm_minor_alloc. */
static struct drm_minor *drm_minor_create_at(struct drm_device *dev, int type, int index, const char *name)
{
    struct drm_minor *minor = malloc(sizeof(*minor));

    if (minor == NULL) { return NULL; }
    memset(minor, 0, sizeof(*minor));

    minor->index            = index;
    minor->type             = type;
    minor->dev              = dev;
    minor->device_node_name = strdup(name);
    if (minor->device_node_name == NULL) {
        free(minor);
        return NULL;
    }

    return minor;
}

static void drm_minor_destroy(struct drm_minor *minor)
{
    if (minor == NULL) { return; }

    drm_minor_free(minor->type, minor->index);
    free(minor->device_node_name);
    free(minor);
}

/* Publish one node under /dev/dri, wired to the DRM file operations. */
static void drm_register_node(struct drm_device *dev, struct drm_minor *minor, uint64_t devt, bool primary)
{
    tmpfs_device_ops_t ops;
    char               path[64];
    int                ret;

    memset(&ops, 0, sizeof(ops));
    ops.open       = (tmpfs_dev_open_t)drm_dev_open;
    ops.release    = (tmpfs_dev_release_t)drm_dev_release;
    ops.mmap       = drm_dev_file_mmap;
    ops.file_read  = drm_dev_file_read;
    ops.file_write = drm_dev_file_write;
    ops.file_poll  = drm_dev_file_poll;
    ops.file_ioctl = drm_dev_file_ioctl;
    ops.ctx        = dev;

    snprintf(path, sizeof(path), "/dev/dri/%s", minor->device_node_name);

    ret = devtmpfs_register_char_device(path, devt, devt, file_stream, &ops);
    if (ret != 0) {
        DRM_ERROR("Failed to register %s: %d\n", path, ret);
        return;
    }

    if (primary) {
        dev->dev_node_card0 = (void *)(uintptr_t)1;
    } else {
        dev->dev_node_renderD_unused = (void *)(uintptr_t)1;
    }
}

struct drm_device *drm_dev_alloc(struct drm_driver *driver)
{
    struct drm_device *dev;
    char               name[32];
    int                primary_idx;
    int                ret;

    if (driver == NULL) { return NULL; }

    dev = malloc(sizeof(*dev));
    if (dev == NULL) { return NULL; }
    memset(dev, 0, sizeof(*dev));

    dev->driver                 = driver;
    dev->num_crtc               = 0;
    dev->vblank_disable_allowed = true;
    dev->refcount               = 1; /* the caller's reference */

    ilist_init(&dev->filelist);

    /* Before any driver KMS setup: KMS objects attach these properties as
     * they are created. */
    ret = drm_mode_config_init(dev);
    if (ret != 0) {
        DRM_ERROR("Failed to initialise KMS mode configuration: %d\n", ret);
        free(dev);
        return NULL;
    }

    primary_idx = drm_minor_alloc(DRM_MINOR_PRIMARY);
    if (primary_idx < 0) {
        drm_mode_config_cleanup(dev);
        free(dev);
        return NULL;
    }
    snprintf(name, sizeof(name), "card%d", primary_idx);
    dev->primary = drm_minor_create_at(dev, DRM_MINOR_PRIMARY, primary_idx, name);
    if (dev->primary == NULL) {
        drm_minor_free(DRM_MINOR_PRIMARY, primary_idx);
        drm_mode_config_cleanup(dev);
        free(dev);
        return NULL;
    }

    {
        int render_idx = drm_minor_alloc(DRM_MINOR_RENDER);

        if (render_idx < 0) {
            drm_minor_destroy(dev->primary);
            dev->primary = NULL;
            drm_mode_config_cleanup(dev);
            free(dev);
            return NULL;
        }

        /* Render nodes start at 128, matching Linux. */
        snprintf(name, sizeof(name), "renderD%d", 128 + render_idx);
        dev->render = drm_minor_create_at(dev, DRM_MINOR_RENDER, render_idx, name);
        if (dev->render == NULL) {
            drm_minor_free(DRM_MINOR_RENDER, render_idx);
            drm_minor_destroy(dev->primary);
            dev->primary = NULL;
            drm_mode_config_cleanup(dev);
            free(dev);
            return NULL;
        }
    }

    return dev;
}

int drm_dev_register(struct drm_device *dev, uint64_t flags)
{
    (void)flags;

    if (dev == NULL) { return -EINVAL; }

    dev->mode_config.min_width  = 0;
    dev->mode_config.min_height = 0;
    dev->mode_config.max_width  = 8192;
    dev->mode_config.max_height = 8192;

    if (dev->driver != NULL && (dev->driver->driver_features & DRIVER_MODESET) != 0) {
        dev->mode_config.cursor_width               = 64;
        dev->mode_config.cursor_height              = 64;
        dev->mode_config.async_page_flip            = false;
        dev->mode_config.fb_modifiers_not_supported = false;
        dev->mode_config.normalize_zpos             = true;
        dev->mode_config.poll_enabled               = true;
    }

    DRM_INFO("Initialized %s %d.%d.%d %s\n", dev->driver->name, dev->driver->major, dev->driver->minor,
             dev->driver->patchlevel, dev->driver->date);

    /* /sys/class/drm/cardN, which is how libdrm finds the device node. */
    if (drm_class_registered && dev->primary != NULL) {
        struct device *ddev =
            device_create(&drm_class, NULL, MKDEV(226, dev->primary->index), dev, "card%d", dev->primary->index);

        if (ddev != NULL) { DRM_INFO("Created /sys/class/drm/%s\n", kobject_name(&ddev->kobj)); }
    }

    if (dev->primary != NULL) { drm_register_node(dev, dev->primary, MKDEV(226, dev->primary->index), true); }

    /* Only drivers that say they can render get a render node: it exists
     * precisely to hand out render-only access. */
    if (dev->render != NULL && (dev->driver->driver_features & DRIVER_RENDER) != 0) {
        drm_register_node(dev, dev->render, MKDEV(226, 128 + dev->render->index), false);
    }

    return 0;
}

void drm_dev_unregister(struct drm_device *dev)
{
    if (dev == NULL) { return; }

    drm_dev_put(dev);
}

/* ------------------------------------------------------------- refcounting */

struct drm_device *drm_dev_get(struct drm_device *dev)
{
    if (dev == NULL) { return NULL; }

    spin_lock(&dev->ref_lock);
    if (dev->unplugged) {
        spin_unlock(&dev->ref_lock);
        return NULL;
    }
    dev->refcount++;
    spin_unlock(&dev->ref_lock);

    return dev;
}

void drm_dev_put(struct drm_device *dev)
{
    int remaining;

    if (dev == NULL) { return; }

    spin_lock(&dev->ref_lock);
    remaining = --dev->refcount;
    spin_unlock(&dev->ref_lock);

    if (remaining != 0) { return; }

    /* Out of the global list first: nothing may find it from here on. */
    {
        extern void drm_device_list_remove(struct drm_device *d);

        drm_device_list_remove(dev);
    }

    if (dev->driver != NULL && dev->driver->release != NULL) { dev->driver->release(dev); }

    drm_minor_destroy(dev->primary);
    dev->primary = NULL;
    drm_minor_destroy(dev->render);
    dev->render = NULL;

    free(dev->unique);
    free(dev->busid_str);
    free(dev);
}

void drm_dev_unplug(struct drm_device *dev)
{
    if (dev == NULL) { return; }

    spin_lock(&dev->ref_lock);
    dev->unplugged = 1;
    spin_unlock(&dev->ref_lock);
}

/* ------------------------------------------------------------- open/close */

int drm_open(struct drm_device *dev, struct drm_file *file)
{
    int ret;

    if (dev == NULL || file == NULL) { return -EINVAL; }

    /* The device must outlive every open file on it. */
    if (drm_dev_get(dev) == NULL) { return -ENODEV; }

    memset(file, 0, sizeof(*file));

    drm_idr_init(&file->object_idr);
    ilist_init(&file->fbs_head);
    ilist_init(&file->object_list);

    ret = drm_ht_create(&file->magiclist, 4);
    if (ret != 0) {
        drm_idr_destroy(&file->object_idr);
        drm_dev_put(dev);
        return ret;
    }

    file->authenticated        = false;
    file->universal_planes     = false;
    file->atomic               = false;
    file->aspect_ratio_allowed = false;
    file->event_closing        = false;
    wait_queue_init(&file->event_wait);

    file->minor_unused = dev; /* how drm_release finds the device back */

    spin_lock(&dev->filelist_lock);
    ilist_insert_after(&dev->filelist, &file->head);
    dev->open_count++;
    spin_unlock(&dev->filelist_lock);

    if (dev->driver != NULL && dev->driver->open != NULL) {
        ret = dev->driver->open(dev, file);
        if (ret != 0) {
            spin_lock(&dev->filelist_lock);
            ilist_remove(&file->head);
            dev->open_count--;
            spin_unlock(&dev->filelist_lock);
            drm_ht_destroy(&file->magiclist);
            drm_idr_destroy(&file->object_idr);
            drm_dev_put(dev);
            return ret;
        }
    }

    return 0;
}

/*
 * Closing: cancel anything pending, tell the driver, then give everything
 * back in dependency order -- framebuffers (which hold buffer references)
 * before GEM handles (which hold the buffers themselves).
 */
void drm_release(struct drm_file *file)
{
    struct drm_device *dev;

    if (file == NULL) { return; }

    dev = (struct drm_device *)file->minor_unused;

    if (dev != NULL) {
        drm_vblank_cancel_pending(dev, file);

        spin_lock(&dev->filelist_lock);
        ilist_remove(&file->head);
        dev->open_count--;
        spin_unlock(&dev->filelist_lock);

        if (dev->driver != NULL && dev->driver->postclose != NULL) { dev->driver->postclose(dev, file); }
        if (dev->open_count == 0 && dev->driver != NULL && dev->driver->lastclose != NULL) {
            dev->driver->lastclose(dev);
        }
    } else {
        ilist_remove(&file->head);
    }

    while (file->fbs_head.next != NULL && file->fbs_head.next != &file->fbs_head) {
        struct drm_framebuffer *fb    = container_of(file->fbs_head.next, struct drm_framebuffer, filp_head);
        uint32_t                fb_id = fb->base.id;

        /* rmfb does the full teardown; if it refuses, fall back to just
         * dropping this file's claim on it. */
        if (drm_mode_rmfb(dev, &fb_id, file) != 0) {
            drm_framebuffer_cleanup(fb);
            free(fb);
        }
    }

    {
        ilist_node_t *node = file->object_list.next;

        while (node != NULL && node != &file->object_list) {
            struct drm_gem_handle_entry *entry = container_of(node, struct drm_gem_handle_entry, head);
            struct drm_gem_object       *obj   = entry->obj;

            node = node->next;

            spin_lock(&file->table_lock);
            drm_idr_remove(&file->object_idr, entry->handle);
            ilist_remove(&entry->head);
            spin_unlock(&file->table_lock);

            obj->handle_count--;
            drm_gem_object_put(obj);
            free(entry);
        }
    }

    drm_file_free(file);

    if (dev != NULL) { drm_dev_put(dev); }
}
