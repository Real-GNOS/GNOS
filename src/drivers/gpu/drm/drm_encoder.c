/*
 * drm_encoder.c - the link between a CRTC and a connector. (GPLv2)
 *
 * An encoder takes the pixel stream a CRTC produces and turns it into
 * whatever the connector's cable wants: TMDS for HDMI and DVI, LVDS for a
 * laptop panel, and so on.  There is deliberately no state to speak of
 * here -- which CRTC feeds it and which connector it feeds are decided at
 * modeset time -- so this file is mostly registration and the one ioctl
 * that reports what an encoder can be wired to.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_fourcc.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* From drm_mode_object.c. */
extern int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

int drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder, void *funcs, int encoder_type, const char *name)
{
    int ret;

    (void)name;

    if (dev == NULL || encoder == NULL) { return -EINVAL; }

    ret = drm_mode_object_idr_alloc(dev, &encoder->base, DRM_MODE_OBJECT_ENCODER);
    if (ret != 0) { return ret; }

    ilist_insert_after(&dev->mode_config.encoder_list, &encoder->head);

    encoder->dev             = dev;
    encoder->encoder_type    = (uint32_t)encoder_type;
    encoder->possible_crtcs  = 0; /* filled in by the driver */
    encoder->possible_clones = 0;
    encoder->crtc            = NULL;
    encoder->helper_private  = funcs;

    dev->mode_config.num_encoder++;

    return 0;
}

/*
 * DRM_IOCTL_MODE_GETENCODER: what kind of encoder this is, what is feeding
 * it now, and what could feed it.  @possible_crtcs is a bitmask over the
 * device's CRTCs, which is how a client works out legal wirings without
 * asking the driver.
 */
int drm_mode_getencoder(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_encoder *req = (struct drm_mode_get_encoder *)data;
    struct drm_mode_object      *obj;
    struct drm_encoder          *encoder;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, req->encoder_id, DRM_MODE_OBJECT_ENCODER);
    if (obj == NULL) { return -ENOENT; }
    encoder = container_of(obj, struct drm_encoder, base);

    req->encoder_type    = encoder->encoder_type;
    req->crtc_id         = (encoder->crtc != NULL) ? encoder->crtc->base.id : 0;
    req->possible_crtcs  = encoder->possible_crtcs;
    req->possible_clones = encoder->possible_clones;

    drm_mode_object_put(obj);
    return 0;
}

void drm_encoder_cleanup(struct drm_encoder *encoder)
{
    struct drm_device *dev;

    if (encoder == NULL) { return; }

    dev = encoder->dev;

    ilist_remove(&encoder->head);

    if (dev != NULL) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, encoder->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_encoder > 0) { dev->mode_config.num_encoder--; }
    }
}
