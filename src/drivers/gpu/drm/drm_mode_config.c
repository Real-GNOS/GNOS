/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_mode_config.c - the device's display bookkeeping. (GPLv2)
 *
 * Every device has one drm_mode_config: the lists of CRTCs, encoders,
 * connectors, planes, framebuffers, properties and blobs it owns, the id
 * tables they are published in, the limits on what geometries it accepts,
 * and the standard properties every atomic client expects to find.
 *
 * Setting it up means creating those standard properties -- the things
 * MODE_ATOMIC refers to by name (FB_ID, CRTC_ID, MODE_ID, SRC_*, CRTC_*,
 * and so on).  Tearing it down means undoing all of it in the order that
 * respects who references whom: framebuffers first, because they hold
 * references to buffers, then the objects that point at framebuffers.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_fourcc.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_port.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define DRM_S32_MAX ((int32_t)0x7fffffff)
#define DRM_S32_MIN (-DRM_S32_MAX - 1)

/* From drm_property.c. */
extern void drm_property_destroy(struct drm_device *dev, struct drm_property *property);
extern void drm_property_blob_put(struct drm_property_blob *blob);

/* Per-object cleanup, implemented by the module that owns each type. */
extern void drm_crtc_cleanup(struct drm_crtc *crtc);
extern void drm_connector_cleanup(struct drm_connector *connector);
extern void drm_encoder_cleanup(struct drm_encoder *encoder);
extern void drm_plane_cleanup(struct drm_plane *plane);
extern void drm_framebuffer_cleanup(struct drm_framebuffer *fb);

/* A property whose value is the id of another KMS object. */
static struct drm_property *drm_object_property(struct drm_device *dev, const char *name, uint32_t object_type)
{
    struct drm_property *prop = drm_property_create(dev, DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC, name, 1);

    if (prop != NULL) { prop->values[0] = object_type; }
    return prop;
}

/* A property accepting any signed 32-bit value (positions may be negative). */
static struct drm_property *drm_signed_property(struct drm_device *dev, const char *name, int32_t min, int32_t max)
{
    struct drm_property *prop = drm_property_create(dev, DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC, name, 2);

    if (prop != NULL) {
        prop->values[0] = (uint64_t)(int64_t)min;
        prop->values[1] = (uint64_t)(int64_t)max;
    }
    return prop;
}

int drm_mode_config_init(struct drm_device *dev)
{
    if (dev == NULL) { return -EINVAL; }

    memset(&dev->mode_config.mutex, 0, sizeof(dev->mode_config.mutex));
    memset(&dev->mode_config.idr_mutex, 0, sizeof(dev->mode_config.idr_mutex));
    memset(&dev->mode_config.fb_lock, 0, sizeof(dev->mode_config.fb_lock));
    memset(&dev->mode_config.blob_lock, 0, sizeof(dev->mode_config.blob_lock));
    memset(&dev->mode_config.commit_queue_lock, 0, sizeof(dev->mode_config.commit_queue_lock));
    wait_queue_init(&dev->mode_config.commit_queue_wait);
    dev->mode_config.commit_queue_next = 0;
    dev->mode_config.commit_queue_done = 0;

    drm_idr_init(&dev->mode_config.object_idr);
    drm_idr_init(&dev->mode_config.fb_idr);

    ilist_init(&dev->mode_config.fb_list);
    ilist_init(&dev->mode_config.crtc_list);
    ilist_init(&dev->mode_config.connector_list);
    ilist_init(&dev->mode_config.encoder_list);
    ilist_init(&dev->mode_config.plane_list);
    ilist_init(&dev->mode_config.property_list);
    ilist_init(&dev->mode_config.property_blob_list);
    ilist_init(&dev->mode_config.private_obj_list);

    dev->mode_config.num_connector               = 0;
    dev->mode_config.num_encoder                 = 0;
    dev->mode_config.num_crtc                    = 0;
    dev->mode_config.num_plane                   = 0;
    dev->mode_config.num_total_plane             = 0;
    dev->mode_config.num_fb                      = 0;
    dev->mode_config.num_connector_property_list = 0;

    /* Ballpark limits; a driver narrows them to what it can really scan. */
    dev->mode_config.min_width     = 0;
    dev->mode_config.min_height    = 0;
    dev->mode_config.max_width     = 8192;
    dev->mode_config.max_height    = 8192;
    dev->mode_config.cursor_width  = 64;
    dev->mode_config.cursor_height = 64;

    dev->mode_config.async_page_flip                             = false;
    dev->mode_config.fb_modifiers_not_supported                  = false;
    dev->mode_config.normalize_zpos                              = true;
    dev->mode_config.atomic_async_page_flip_not_supported_unused = false;
    dev->mode_config.poll_enabled                                = false;
    dev->mode_config.poll_running                                = false;
    dev->mode_config.delayed_event                               = false;
    dev->mode_config.poll_init                                   = false;

    dev->mode_config.poll_work_unused = NULL;
    dev->mode_config.helper_private   = NULL;

    /* Every property slot starts empty; the ones below are the standard
     * set an atomic client expects to find on this device. */
    dev->mode_config.prop_src_x                   = NULL;
    dev->mode_config.prop_src_y                   = NULL;
    dev->mode_config.prop_src_w                   = NULL;
    dev->mode_config.prop_src_h                   = NULL;
    dev->mode_config.prop_crtc_x                  = NULL;
    dev->mode_config.prop_crtc_y                  = NULL;
    dev->mode_config.prop_crtc_w                  = NULL;
    dev->mode_config.prop_crtc_h                  = NULL;
    dev->mode_config.prop_fb_id                   = NULL;
    dev->mode_config.prop_in_fence_fd             = NULL;
    dev->mode_config.prop_out_fence_ptr           = NULL;
    dev->mode_config.prop_crtc_id                 = NULL;
    dev->mode_config.prop_active                  = NULL;
    dev->mode_config.prop_mode_id                 = NULL;
    dev->mode_config.prop_plane_type              = NULL;
    dev->mode_config.prop_zpos                    = NULL;
    dev->mode_config.prop_zpos_default            = NULL;
    dev->mode_config.prop_rotation                = NULL;
    dev->mode_config.prop_pixel_blend_mode        = NULL;
    dev->mode_config.prop_src_blend_pixel_unused  = NULL;
    dev->mode_config.prop_alpha                   = NULL;
    dev->mode_config.prop_connector_id            = NULL;
    dev->mode_config.prop_dpms                    = NULL;
    dev->mode_config.prop_path                    = NULL;
    dev->mode_config.prop_tile                    = NULL;
    dev->mode_config.prop_link_status             = NULL;
    dev->mode_config.prop_edid                    = NULL;
    dev->mode_config.prop_content_protection      = NULL;
    dev->mode_config.prop_scaling_mode            = NULL;
    dev->mode_config.prop_aspect_ratio            = NULL;
    dev->mode_config.prop_vrr_capable             = NULL;
    dev->mode_config.prop_hdr_output_metadata     = NULL;
    dev->mode_config.prop_aspect_ratio_unused     = NULL;
    dev->mode_config.prop_gamma_lut               = NULL;
    dev->mode_config.prop_degamma_lut             = NULL;
    dev->mode_config.prop_ctm                     = NULL;
    dev->mode_config.prop_gamma_lut_size          = NULL;
    dev->mode_config.prop_degamma_lut_size        = NULL;
    dev->mode_config.prop_ctm_size                = NULL;
    dev->mode_config.prop_max_bpc                 = NULL;
    dev->mode_config.prop_color_mode_unused       = NULL;
    dev->mode_config.prop_colorspace              = NULL;
    dev->mode_config.prop_writeback_fb_id         = NULL;
    dev->mode_config.prop_writeback_pix_fmt       = NULL;
    dev->mode_config.prop_writeback_out_fence_ptr = NULL;

    /* Plane properties: what is shown, where it comes from, where it goes. */
    dev->mode_config.prop_fb_id   = drm_object_property(dev, "FB_ID", DRM_MODE_OBJECT_FB);
    dev->mode_config.prop_crtc_id = drm_object_property(dev, "CRTC_ID", DRM_MODE_OBJECT_CRTC);
    dev->mode_config.prop_active  = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "ACTIVE", 0, 1);
    dev->mode_config.prop_mode_id = drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC, "MODE_ID", 0);
    dev->mode_config.prop_src_x   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_X", 0, UINT32_MAX);
    dev->mode_config.prop_src_y   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_Y", 0, UINT32_MAX);
    dev->mode_config.prop_src_w   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_W", 0, UINT32_MAX);
    dev->mode_config.prop_src_h   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "SRC_H", 0, UINT32_MAX);
    dev->mode_config.prop_crtc_x  = drm_signed_property(dev, "CRTC_X", DRM_S32_MIN, DRM_S32_MAX);
    dev->mode_config.prop_crtc_y  = drm_signed_property(dev, "CRTC_Y", DRM_S32_MIN, DRM_S32_MAX);
    dev->mode_config.prop_crtc_w  = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "CRTC_W", 0, UINT32_MAX);
    dev->mode_config.prop_crtc_h  = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "CRTC_H", 0, UINT32_MAX);
    dev->mode_config.prop_zpos    = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "zpos", 0, 255);
    dev->mode_config.prop_alpha   = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, "alpha", 0, UINT16_MAX);

    {
        static const struct drm_mode_property_enum plane_types[] = {
            {DRM_PLANE_TYPE_OVERLAY, "Overlay"},
            {DRM_PLANE_TYPE_PRIMARY, "Primary"},
            {DRM_PLANE_TYPE_CURSOR,  "Cursor" },
        };
        dev->mode_config.prop_plane_type =
            drm_property_create_enum(dev, DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_ATOMIC, "type", plane_types, 3);
    }

    /* Any one of these missing means every atomic commit would fail in a
     * confusing way later, so fail here instead. */
    if (dev->mode_config.prop_fb_id == NULL || dev->mode_config.prop_crtc_id == NULL
        || dev->mode_config.prop_active == NULL || dev->mode_config.prop_mode_id == NULL
        || dev->mode_config.prop_src_x == NULL || dev->mode_config.prop_src_y == NULL
        || dev->mode_config.prop_src_w == NULL || dev->mode_config.prop_src_h == NULL
        || dev->mode_config.prop_crtc_x == NULL || dev->mode_config.prop_crtc_y == NULL
        || dev->mode_config.prop_crtc_w == NULL || dev->mode_config.prop_crtc_h == NULL
        || dev->mode_config.prop_zpos == NULL || dev->mode_config.prop_alpha == NULL
        || dev->mode_config.prop_plane_type == NULL) {
        drm_mode_config_cleanup(dev);
        return -ENOMEM;
    }

    return 0;
}

/*
 * Walk a list of KMS objects, hand each to @cleanup, then empty the list.
 * The next node is read before the callback runs, because cleaning an
 * object up unlinks it.
 */
static void __attribute__((unused)) drm_mode_config_cleanup_list(ilist_node_t *list, void (*cleanup)(void *obj))
{
    ilist_node_t *node = list->next;

    while (node != NULL && node != list) {
        ilist_node_t *next = node->next;

        if (cleanup != NULL) { cleanup(node); }
        node = next;
    }

    ilist_init(list);
}

void drm_mode_config_cleanup(struct drm_device *dev)
{
    if (dev == NULL) { return; }

    /* Framebuffers first: they hold references to the buffers behind them. */
    {
        ilist_node_t *node = dev->mode_config.fb_list.next;

        while (node != NULL && node != &dev->mode_config.fb_list) {
            ilist_node_t          *next = node->next;
            struct drm_framebuffer *fb  = container_of(node, struct drm_framebuffer, head);

            drm_framebuffer_cleanup(fb);
            free(fb);
            node = next;
        }
        ilist_init(&dev->mode_config.fb_list);
    }

    /* Then everything that could be pointing at one. */
    {
        ilist_node_t *node = dev->mode_config.plane_list.next;

        while (node != NULL && node != &dev->mode_config.plane_list) {
            ilist_node_t     *next  = node->next;
            struct drm_plane *plane = container_of(node, struct drm_plane, head);

            drm_plane_cleanup(plane);
            node = next;
        }
        ilist_init(&dev->mode_config.plane_list);
    }

    {
        ilist_node_t *node = dev->mode_config.crtc_list.next;

        while (node != NULL && node != &dev->mode_config.crtc_list) {
            ilist_node_t    *next = node->next;
            struct drm_crtc *crtc = container_of(node, struct drm_crtc, head);

            drm_crtc_cleanup(crtc);
            node = next;
        }
        ilist_init(&dev->mode_config.crtc_list);
    }

    {
        ilist_node_t *node = dev->mode_config.connector_list.next;

        while (node != NULL && node != &dev->mode_config.connector_list) {
            ilist_node_t          *next      = node->next;
            struct drm_connector  *connector = container_of(node, struct drm_connector, head);

            drm_connector_cleanup(connector);
            node = next;
        }
        ilist_init(&dev->mode_config.connector_list);
    }

    {
        ilist_node_t *node = dev->mode_config.encoder_list.next;

        while (node != NULL && node != &dev->mode_config.encoder_list) {
            ilist_node_t       *next    = node->next;
            struct drm_encoder *encoder = container_of(node, struct drm_encoder, head);

            drm_encoder_cleanup(encoder);
            node = next;
        }
        ilist_init(&dev->mode_config.encoder_list);
    }

    {
        ilist_node_t *node = dev->mode_config.property_list.next;

        while (node != NULL && node != &dev->mode_config.property_list) {
            ilist_node_t       *next = node->next;
            struct drm_property *prop = container_of(node, struct drm_property, dev_head);

            drm_property_destroy(dev, prop);
            node = next;
        }
        ilist_init(&dev->mode_config.property_list);
    }

    {
        ilist_node_t *node = dev->mode_config.property_blob_list.next;

        while (node != NULL && node != &dev->mode_config.property_blob_list) {
            ilist_node_t           *next = node->next;
            struct drm_property_blob *blob = container_of(node, struct drm_property_blob, head_global);

            drm_property_blob_put(blob);
            node = next;
        }
        ilist_init(&dev->mode_config.property_blob_list);
    }

    drm_idr_destroy(&dev->mode_config.object_idr);
    drm_idr_destroy(&dev->mode_config.fb_idr);

    dev->mode_config.num_connector   = 0;
    dev->mode_config.num_encoder     = 0;
    dev->mode_config.num_crtc        = 0;
    dev->mode_config.num_plane       = 0;
    dev->mode_config.num_total_plane = 0;
    dev->mode_config.num_fb          = 0;
}

/*
 * DRM_IOCTL_MODE_GETRESOURCES: the inventory a client starts from.  Call it
 * with zero counts to learn how much space to allocate, then again with the
 * arrays; we copy no more than the caller has room for and always report
 * the real totals.
 */
int drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)data;
    uint32_t                  wanted_fbs, wanted_crtcs, wanted_connectors, wanted_encoders;
    uint32_t                 *fbs = NULL, *crtcs = NULL, *connectors = NULL, *encoders = NULL;
    uint32_t                  n;
    ilist_node_t             *node;

    (void)file_priv;

    if (dev == NULL || res == NULL) { return -EINVAL; }

    wanted_fbs        = res->count_fbs;
    wanted_crtcs      = res->count_crtcs;
    wanted_connectors = res->count_connectors;
    wanted_encoders   = res->count_encoders;

    if (dev->mode_config.num_fb != 0) { fbs = malloc((size_t)dev->mode_config.num_fb * sizeof(*fbs)); }
    if (dev->mode_config.num_crtc != 0) { crtcs = malloc((size_t)dev->mode_config.num_crtc * sizeof(*crtcs)); }
    if (dev->mode_config.num_connector != 0) { connectors = malloc((size_t)dev->mode_config.num_connector * sizeof(*connectors)); }
    if (dev->mode_config.num_encoder != 0) { encoders = malloc((size_t)dev->mode_config.num_encoder * sizeof(*encoders)); }

    if ((dev->mode_config.num_fb != 0 && fbs == NULL) || (dev->mode_config.num_crtc != 0 && crtcs == NULL)
        || (dev->mode_config.num_connector != 0 && connectors == NULL)
        || (dev->mode_config.num_encoder != 0 && encoders == NULL)) {
        free(fbs);
        free(crtcs);
        free(connectors);
        free(encoders);
        return -ENOMEM;
    }

    n = 0;
    for (node = dev->mode_config.fb_list.next; node != &dev->mode_config.fb_list; node = node->next) {
        fbs[n++] = container_of(node, struct drm_framebuffer, head)->base.id;
    }
    n = 0;
    for (node = dev->mode_config.crtc_list.next; node != &dev->mode_config.crtc_list; node = node->next) {
        crtcs[n++] = container_of(node, struct drm_crtc, head)->base.id;
    }
    n = 0;
    for (node = dev->mode_config.connector_list.next; node != &dev->mode_config.connector_list; node = node->next) {
        connectors[n++] = container_of(node, struct drm_connector, head)->base.id;
    }
    n = 0;
    for (node = dev->mode_config.encoder_list.next; node != &dev->mode_config.encoder_list; node = node->next) {
        encoders[n++] = container_of(node, struct drm_encoder, head)->base.id;
    }

    /* Copy out whichever arrays the caller asked for and has room for. */
    {
        uint32_t give_fbs        = (wanted_fbs < (uint32_t)dev->mode_config.num_fb) ? wanted_fbs : (uint32_t)dev->mode_config.num_fb;
        uint32_t give_crtcs      = (wanted_crtcs < (uint32_t)dev->mode_config.num_crtc) ? wanted_crtcs : (uint32_t)dev->mode_config.num_crtc;
        uint32_t give_connectors = (wanted_connectors < (uint32_t)dev->mode_config.num_connector)
                                       ? wanted_connectors
                                       : (uint32_t)dev->mode_config.num_connector;
        uint32_t give_encoders   = (wanted_encoders < (uint32_t)dev->mode_config.num_encoder)
                                       ? wanted_encoders
                                       : (uint32_t)dev->mode_config.num_encoder;
        int      failed          = 0;

        if (give_fbs != 0
            && (res->fb_id_ptr == 0
                || copy_to_user((void *)(uintptr_t)res->fb_id_ptr, fbs, (size_t)give_fbs * sizeof(*fbs)) != 0)) {
            failed = 1;
        }
        if (!failed && give_crtcs != 0
            && (res->crtc_id_ptr == 0
                || copy_to_user((void *)(uintptr_t)res->crtc_id_ptr, crtcs, (size_t)give_crtcs * sizeof(*crtcs)) != 0)) {
            failed = 1;
        }
        if (!failed && give_connectors != 0
            && (res->connector_id_ptr == 0
                || copy_to_user((void *)(uintptr_t)res->connector_id_ptr, connectors,
                                (size_t)give_connectors * sizeof(*connectors)) != 0)) {
            failed = 1;
        }
        if (!failed && give_encoders != 0
            && (res->encoder_id_ptr == 0
                || copy_to_user((void *)(uintptr_t)res->encoder_id_ptr, encoders, (size_t)give_encoders * sizeof(*encoders)) != 0)) {
            failed = 1;
        }

        free(fbs);
        free(crtcs);
        free(connectors);
        free(encoders);

        if (failed) { return -EFAULT; }
    }

    res->min_width        = dev->mode_config.min_width;
    res->max_width        = dev->mode_config.max_width;
    res->min_height       = dev->mode_config.min_height;
    res->max_height       = dev->mode_config.max_height;
    res->count_fbs        = (__u32)dev->mode_config.num_fb;
    res->count_crtcs      = (__u32)dev->mode_config.num_crtc;
    res->count_connectors = (__u32)dev->mode_config.num_connector;
    res->count_encoders   = (__u32)dev->mode_config.num_encoder;

    return 0;
}

/* Managed wrapper: with no resource manager to register with, this is just
 * the plain initialiser under its other name. */
int drmm_mode_config_init(struct drm_device *dev)
{
    if (dev == NULL) { return -EINVAL; }

    return drm_mode_config_init(dev);
}
