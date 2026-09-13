/*
 * drm_connector.c - the socket the monitor is plugged into. (GPLv2)
 *
 * A connector is the one object that corresponds to something a user can
 * see and touch: a VGA port, an HDMI socket, an internal panel.  It owns
 * the list of modes the monitor says it supports, the EDID we read from it,
 * and the answer to "is anything actually connected".  Everything a
 * compositor needs in order to decide what to display comes from this
 * object, which is why GETCONNECTOR is the chattiest ioctl in KMS.
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

/* From drm_mode_object.c, drm_property.c and drm_modes.c. */
extern int                       drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);
extern struct drm_property_blob *drm_property_create_blob(struct drm_device *dev, const void *data, size_t length);
extern void                      drm_property_blob_put(struct drm_property_blob *blob);
extern void                      drm_convert_to_umode(struct drm_mode_modeinfo *out, const struct drm_display_mode *in);

int drm_connector_init(struct drm_device *dev, struct drm_connector *connector, void *funcs, int connector_type)
{
    int ret;

    if (dev == NULL || connector == NULL) { return -EINVAL; }

    ret = drm_mode_object_idr_alloc(dev, &connector->base, DRM_MODE_OBJECT_CONNECTOR);
    if (ret != 0) { return ret; }

    drm_modeset_lock_init(&connector->mutex);

    ilist_init(&connector->modes);
    ilist_init(&connector->user_modes);

    ilist_insert_after(&dev->mode_config.connector_list, &connector->head);

    connector->dev                     = dev;
    connector->connector_type          = (uint32_t)connector_type;
    connector->connector_type_id       = 0;
    connector->status                  = connector_status_unknown;
    connector->force                   = DRM_FORCE_UNSPECIFIED;
    connector->helper_private          = funcs;
    connector->state                   = NULL;
    connector->edid_blob               = NULL;
    connector->path_blob               = NULL;
    connector->tile_blob               = NULL;
    connector->eld                     = NULL;
    connector->edid_blob_ptr           = NULL;
    connector->possible_encoders_count = 0;
    connector->possible_encoders_ids   = NULL;
    connector->interlace_allowed       = false;
    connector->doublescan_allowed      = false;
    connector->stereo_allowed          = false;
    connector->ycbcr_420_allowed       = 0;
    connector->display_info_width_mm   = 0;
    connector->display_info_height_mm  = 0;
    connector->null_edid_counter       = 0;
    connector->override_edid           = false;
    connector->override_edid_set       = false;
    memset(&connector->edid_lock, 0, sizeof(connector->edid_lock));
    memset(connector->name, 0, sizeof(connector->name));

    dev->mode_config.num_connector++;

    /* CRTC_ID is how a client asks which CRTC drives this connector. */
    ret = drm_object_attach_property(&connector->base, dev->mode_config.prop_crtc_id, 0);
    if (ret != 0) {
        drm_connector_cleanup(connector);
        return ret;
    }

    return 0;
}

/* Record that @encoder is one of the encoders that could drive @connector. */
int drm_connector_attach_encoder(struct drm_connector *connector, struct drm_encoder *encoder)
{
    uint32_t *ids;
    uint32_t  count;

    if (connector == NULL || encoder == NULL) { return -EINVAL; }

    count = connector->possible_encoders_count + 1;
    ids   = realloc(connector->possible_encoders_ids, (size_t)count * sizeof(uint32_t));
    if (ids == NULL) { return -ENOMEM; }

    ids[connector->possible_encoders_count] = encoder->base.id;

    connector->possible_encoders_ids   = ids;
    connector->possible_encoders_count = count;

    return 0;
}

int drm_connector_register(struct drm_connector *connector)
{
    if (connector == NULL) { return -EINVAL; }

    /* Connectors are reachable through GETRESOURCES from the moment they
     * are initialised; there is no second registration step yet. */
    return 0;
}

/*
 * DRM_IOCTL_MODE_GETCONNECTOR: modes, possible encoders, properties,
 * connection state and physical size.  Each array is copied only if the
 * caller asked for it and only up to the room it offered; the counts
 * reported back are always the real ones.
 */
int drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_connector *req = (struct drm_mode_get_connector *)data;
    struct drm_mode_object        *obj;
    struct drm_connector          *connector;
    ilist_node_t                  *node;
    uint32_t                       wanted_modes, wanted_encoders, wanted_props;
    int                            mode_count = 0;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    wanted_modes    = req->count_modes;
    wanted_encoders = req->count_encoders;
    wanted_props    = req->count_props;

    obj = drm_mode_object_find(dev, file_priv, req->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (obj == NULL) { return -ENOENT; }
    connector = container_of(obj, struct drm_connector, base);

    for (node = connector->modes.next; node != NULL && node != &connector->modes; node = node->next) { mode_count++; }

    if (wanted_modes != 0 && mode_count != 0) {
        uint32_t                  copying = (wanted_modes < (uint32_t)mode_count) ? wanted_modes : (uint32_t)mode_count;
        struct drm_mode_modeinfo *modes   = malloc((size_t)copying * sizeof(*modes));

        if (modes == NULL) {
            drm_mode_object_put(obj);
            return -ENOMEM;
        }

        node = connector->modes.next;
        for (uint32_t i = 0; i < copying; i++, node = node->next) {
            drm_convert_to_umode(&modes[i], container_of(node, struct drm_display_mode, head));
        }

        if (req->modes_ptr == 0
            || copy_to_user((void *)(uintptr_t)req->modes_ptr, modes, (size_t)copying * sizeof(*modes)) != 0) {
            free(modes);
            drm_mode_object_put(obj);
            return -EFAULT;
        }
        free(modes);
    }

    if (wanted_encoders != 0 && connector->possible_encoders_count != 0) {
        uint32_t copying = (wanted_encoders < connector->possible_encoders_count) ? wanted_encoders
                                                                                 : connector->possible_encoders_count;

        if (req->encoders_ptr == 0
            || copy_to_user((void *)(uintptr_t)req->encoders_ptr, connector->possible_encoders_ids,
                            (size_t)copying * sizeof(*connector->possible_encoders_ids)) != 0) {
            drm_mode_object_put(obj);
            return -EFAULT;
        }
    }

    if (connector->base.properties != NULL && wanted_props != 0) {
        struct drm_property_set *set     = connector->base.properties;
        uint32_t                 copying = 0;
        uint32_t                *ids     = NULL;
        uint64_t                *values  = NULL;

        spin_lock(&set->lock);
        copying = (wanted_props < set->count) ? wanted_props : set->count;
        if (copying != 0) {
            ids    = malloc((size_t)copying * sizeof(*ids));
            values = malloc((size_t)copying * sizeof(*values));
            if (ids != NULL && values != NULL) {
                memcpy(ids, set->ids, (size_t)copying * sizeof(*ids));
                memcpy(values, set->values, (size_t)copying * sizeof(*values));
            }
        }
        spin_unlock(&set->lock);

        if (copying != 0 && (ids == NULL || values == NULL)) {
            free(ids);
            free(values);
            drm_mode_object_put(obj);
            return -ENOMEM;
        }

        if (copying != 0
            && (req->props_ptr == 0 || req->prop_values_ptr == 0
                || copy_to_user((void *)(uintptr_t)req->props_ptr, ids, (size_t)copying * sizeof(*ids)) != 0
                || copy_to_user((void *)(uintptr_t)req->prop_values_ptr, values, (size_t)copying * sizeof(*values)) != 0)) {
            free(ids);
            free(values);
            drm_mode_object_put(obj);
            return -EFAULT;
        }

        free(ids);
        free(values);
    }

    req->encoder_id        = (connector->state != NULL && connector->state->best_encoder != NULL)
                                 ? connector->state->best_encoder->base.id
                                 : 0;
    req->connector_type    = connector->connector_type;
    req->connector_type_id = connector->connector_type_id;
    req->connection        = (__u32)connector->status;
    req->mm_width          = connector->display_info_width_mm;
    req->mm_height         = connector->display_info_height_mm;
    req->subpixel          = 0;
    req->count_modes       = (__u32)mode_count;
    req->count_props       = (connector->base.properties != NULL) ? connector->base.properties->count : 0;
    req->count_encoders    = (__u32)connector->possible_encoders_count;

    drm_mode_object_put(obj);
    return 0;
}

void drm_connector_cleanup(struct drm_connector *connector)
{
    struct drm_device *dev;

    if (connector == NULL) { return; }

    dev = connector->dev;

    /* The modes belong to this connector and nobody else holds them. */
    while (connector->modes.next != NULL && connector->modes.next != &connector->modes) {
        struct drm_display_mode *mode = container_of(connector->modes.next, struct drm_display_mode, head);

        ilist_remove(&mode->head);
        free(mode);
    }

    ilist_remove(&connector->head);

    if (dev != NULL) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, connector->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_connector > 0) { dev->mode_config.num_connector--; }
    }

    free(connector->possible_encoders_ids);
    connector->possible_encoders_ids   = NULL;
    connector->possible_encoders_count = 0;

    if (connector->edid_blob != NULL) {
        drm_property_blob_put(connector->edid_blob);
        connector->edid_blob = NULL;
    }

    if (connector->path_blob != NULL) {
        drm_property_blob_put(connector->path_blob);
        connector->path_blob = NULL;
    }

    if (connector->tile_blob != NULL) {
        drm_property_blob_put(connector->tile_blob);
        connector->tile_blob = NULL;
    }

    free(connector->eld);
    connector->eld = NULL;

    if (connector->base.properties != NULL) {
        drm_property_set_destroy(connector->base.properties);
        free(connector->base.properties);
        connector->base.properties = NULL;
    }
}

/*
 * Replace the EDID blob with one wrapping @edid (or clear it, when @edid is
 * NULL).  The blob is what user space reads to learn the monitor's name,
 * size and preferred mode.
 */
int drm_connector_update_edid_property(struct drm_connector *connector, const unsigned char *edid, size_t size)
{
    struct drm_device        *dev;
    struct drm_property_blob *fresh = NULL;

    if (connector == NULL || connector->dev == NULL) { return -EINVAL; }

    dev = connector->dev;

    if (connector->edid_blob != NULL) {
        drm_property_blob_put(connector->edid_blob);
        connector->edid_blob = NULL;
    }

    if (edid != NULL && size > 0) {
        fresh = drm_property_create_blob(dev, edid, size);
        if (fresh == NULL) { return -ENOMEM; }
    }

    connector->edid_blob = fresh;
    return 0;
}
