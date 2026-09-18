/*
 * drm_atomic_uapi.c - where user space's description of a display becomes
 * kernel state. (GPLv2)
 *
 * MODE_ATOMIC arrives as four parallel arrays: object ids, how many
 * properties each carries, the property ids, and the values.  This file
 * checks each value against what the property allows, then translates each
 * one into the corresponding field of the proposed state -- which is the
 * whole job, and why the file is mostly one long switch on "which standard
 * property is this?".
 *
 * The page-flip and cursor ioctls live here too: they are the older,
 * narrower ways of asking for the same thing, and both are implemented
 * against the same state and helper machinery.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_port.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define DRM_S32_MAX                     ((int32_t)0x7fffffff)
#define DRM_S32_MIN                     (-DRM_S32_MAX - 1)

/* Implemented in the sibling DRM modules. */
extern struct drm_atomic_state    *drm_atomic_state_alloc(struct drm_device *dev);
extern struct drm_crtc_state      *drm_atomic_get_crtc_state(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern struct drm_plane_state     *drm_atomic_get_plane_state(struct drm_atomic_state *state, struct drm_plane *plane);
extern struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state, struct drm_connector *connector);
extern int                         drm_atomic_check_only(struct drm_atomic_state *state);
extern int                         drm_atomic_commit(struct drm_atomic_state *state);
extern int                         drm_atomic_nonblocking_commit(struct drm_atomic_state *state);
extern void                        drm_atomic_state_free(struct drm_atomic_state *state);
extern struct drm_mode_object     *drm_mode_object_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id, uint32_t type);
extern struct drm_framebuffer     *drm_framebuffer_lookup(struct drm_device *dev, struct drm_file *file_priv, uint32_t id);
extern void                        drm_crtc_arm_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e);
extern void                        drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e);
extern void                        drm_handle_vblank(struct drm_device *dev, unsigned int pipe);
extern struct drm_property_blob   *drm_property_lookup_blob(struct drm_device *dev, uint32_t id);
extern void                        drm_property_blob_put(struct drm_property_blob *blob);
extern struct drm_display_mode    *drm_convert_umode(const struct drm_mode_modeinfo *umode);

/* Is @property attached to @obj, and what is it set to at the moment? */
static bool drm_atomic_object_has_property(struct drm_mode_object *obj, uint32_t property_id, uint64_t *current)
{
    struct drm_property_set *set = (obj != NULL) ? obj->properties : NULL;
    bool                     found = false;

    if (set == NULL) { return false; }

    spin_lock(&set->lock);
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->ids[i] == property_id) {
            if (current != NULL) { *current = set->values[i]; }
            found = true;
            break;
        }
    }
    spin_unlock(&set->lock);

    return found;
}

/*
 * Does @value make sense for @prop?  Ranges are bounded, enums must be one
 * of the listed values, an object property must name an object of the right
 * kind that exists, and a blob must name a blob that exists.  Immutables may
 * only be set to what they already are.
 */
static int drm_atomic_validate_property(struct drm_device *dev, struct drm_mode_object *obj, struct drm_property *prop, uint64_t value)
{
    uint64_t current;

    if (!drm_atomic_object_has_property(obj, prop->base.id, &current)) { return -ENOENT; }
    if ((prop->flags & DRM_MODE_PROP_IMMUTABLE) != 0 && current != value) { return -EINVAL; }

    if ((prop->flags & DRM_MODE_PROP_RANGE) != 0) {
        if (prop->num_values != 2 || value < prop->values[0] || value > prop->values[1]) { return -EINVAL; }
    } else if ((prop->flags & DRM_MODE_PROP_SIGNED_RANGE) != 0) {
        int64_t signed_value = (int64_t)value;

        if (prop->num_values != 2 || signed_value < (int64_t)prop->values[0] || signed_value > (int64_t)prop->values[1]) {
            return -EINVAL;
        }
    } else if ((prop->flags & DRM_MODE_PROP_ENUM) != 0) {
        bool found = false;

        for (ilist_node_t *node = prop->enum_list.next; node != &prop->enum_list; node = node->next) {
            struct drm_property_enum *entry = container_of(node, struct drm_property_enum, head);

            if (entry->value == value) {
                found = true;
                break;
            }
        }
        if (!found) { return -EINVAL; }
    } else if ((prop->flags & DRM_MODE_PROP_OBJECT) != 0 && value != 0) {
        struct drm_mode_object *target;

        if (prop->num_values == 0 || value > UINT32_MAX) { return -EINVAL; }
        target = drm_mode_object_find(dev, NULL, (uint32_t)value, (uint32_t)prop->values[0]);
        if (target == NULL) { return -ENOENT; }
        drm_mode_object_put(target);
    } else if ((prop->flags & DRM_MODE_PROP_BLOB) != 0 && value != 0) {
        struct drm_property_blob *blob;

        if (value > UINT32_MAX) { return -EINVAL; }
        blob = drm_property_lookup_blob(dev, (uint32_t)value);
        if (blob == NULL) { return -ENOENT; }
        drm_property_blob_put(blob);
    }

    return 0;
}

/* Move @crtc's CRTC state into @state so it can be marked as disturbed. */
static int drm_atomic_mark_crtc(struct drm_atomic_state *state, struct drm_crtc *crtc, bool connectors)
{
    struct drm_crtc_state *crtc_state = drm_atomic_get_crtc_state(state, crtc);

    if (crtc_state == NULL) { return -ENOMEM; }

    if (connectors) {
        crtc_state->connectors_changed = true;
    } else {
        crtc_state->planes_changed = true;
    }

    return 0;
}

/* Apply one property value to the proposed state of @obj. */
static int drm_atomic_set_uapi_property(struct drm_atomic_state *state, struct drm_file *file_priv, struct drm_mode_object *obj,
                                        struct drm_property *prop, uint64_t value)
{
    struct drm_mode_config *config = &state->dev->mode_config;

    if (obj->type == DRM_MODE_OBJECT_CRTC) {
        struct drm_crtc       *crtc = container_of(obj, struct drm_crtc, base);
        struct drm_crtc_state *s    = drm_atomic_get_crtc_state(state, crtc);

        if (s == NULL) { return -ENOMEM; }

        if (prop == config->prop_active) {
            s->active         = (value != 0);
            s->enable         = (value != 0);
            s->active_changed = (crtc->state != NULL) ? (s->active != crtc->state->active) : true;
            return 0;
        }

        if (prop == config->prop_mode_id) {
            memset(&s->mode, 0, sizeof(s->mode));

            if (value != 0) {
                /* A mode arrives as a blob holding one drm_mode_modeinfo. */
                struct drm_property_blob *blob = drm_property_lookup_blob(state->dev, (uint32_t)value);
                struct drm_display_mode  *mode;

                if (blob == NULL) { return -ENOENT; }
                if (blob->length != sizeof(struct drm_mode_modeinfo)) {
                    drm_property_blob_put(blob);
                    return -EINVAL;
                }

                mode = drm_convert_umode((const struct drm_mode_modeinfo *)blob->data);
                drm_property_blob_put(blob);
                if (mode == NULL) { return -ENOMEM; }

                memcpy(&s->mode, mode, sizeof(*mode));
                free(mode);
            }

            s->mode_changed = true;
            return 0;
        }
    } else if (obj->type == DRM_MODE_OBJECT_PLANE) {
        struct drm_plane       *plane = container_of(obj, struct drm_plane, base);
        struct drm_plane_state *s     = drm_atomic_get_plane_state(state, plane);
        int32_t                 extent;

        if (s == NULL) { return -ENOMEM; }

        if (prop == config->prop_fb_id) {
            s->fb = (value != 0) ? drm_framebuffer_lookup(state->dev, file_priv, (uint32_t)value) : NULL;
            if (value != 0 && s->fb == NULL) { return -ENOENT; }
            if (s->crtc != NULL) { return drm_atomic_mark_crtc(state, s->crtc, false); }
            return 0;
        }

        if (prop == config->prop_crtc_id) {
            struct drm_crtc        *old_crtc = s->crtc;
            struct drm_mode_object *target =
                (value != 0) ? drm_mode_object_find(state->dev, file_priv, (uint32_t)value, DRM_MODE_OBJECT_CRTC) : NULL;

            if (value != 0 && target == NULL) { return -ENOENT; }

            s->crtc = (target != NULL) ? container_of(target, struct drm_crtc, base) : NULL;
            if (target != NULL) { drm_mode_object_put(target); }

            /* Both ends move: the CRTC the plane left and the one it joins
             * have to recompose. */
            if (old_crtc != NULL) {
                int ret = drm_atomic_mark_crtc(state, old_crtc, false);
                if (ret != 0) { return ret; }
            }
            if (s->crtc != NULL && s->crtc != old_crtc) { return drm_atomic_mark_crtc(state, s->crtc, false); }
            return 0;
        }

        /* Source coordinates are 16.16 fixed point; destination ones are
         * pixels.  Both are moved by their top-left corner, keeping the
         * size, so a client can reposition a plane with one property. */
        if (prop == config->prop_src_x) {
            extent = s->src.x2 - s->src.x1;
            if (value > DRM_S32_MAX || (int64_t)value + extent > DRM_S32_MAX) { return -EINVAL; }
            s->src.x1 = (int32_t)value;
            s->src.x2 = s->src.x1 + extent;
            return 0;
        }
        if (prop == config->prop_src_y) {
            extent = s->src.y2 - s->src.y1;
            if (value > DRM_S32_MAX || (int64_t)value + extent > DRM_S32_MAX) { return -EINVAL; }
            s->src.y1 = (int32_t)value;
            s->src.y2 = s->src.y1 + extent;
            return 0;
        }
        if (prop == config->prop_src_w) {
            if (value > DRM_S32_MAX || (int64_t)s->src.x1 + value > DRM_S32_MAX) { return -EINVAL; }
            s->src.x2 = s->src.x1 + (int32_t)value;
            return 0;
        }
        if (prop == config->prop_src_h) {
            if (value > DRM_S32_MAX || (int64_t)s->src.y1 + value > DRM_S32_MAX) { return -EINVAL; }
            s->src.y2 = s->src.y1 + (int32_t)value;
            return 0;
        }

        /* Destination coordinates may legitimately be negative: a plane can
         * hang off the edge of the screen. */
        if (prop == config->prop_crtc_x) {
            int32_t v = (int32_t)value;

            extent = s->dst.x2 - s->dst.x1;
            if ((int64_t)v + extent > DRM_S32_MAX || (int64_t)v + extent < DRM_S32_MIN) { return -EINVAL; }
            s->dst.x1 = v;
            s->dst.x2 = v + extent;
            return 0;
        }
        if (prop == config->prop_crtc_y) {
            int32_t v = (int32_t)value;

            extent = s->dst.y2 - s->dst.y1;
            if ((int64_t)v + extent > DRM_S32_MAX || (int64_t)v + extent < DRM_S32_MIN) { return -EINVAL; }
            s->dst.y1 = v;
            s->dst.y2 = v + extent;
            return 0;
        }
        if (prop == config->prop_crtc_w) {
            if (value > DRM_S32_MAX || (int64_t)s->dst.x1 + value > DRM_S32_MAX) { return -EINVAL; }
            s->dst.x2 = s->dst.x1 + (int32_t)value;
            return 0;
        }
        if (prop == config->prop_crtc_h) {
            if (value > DRM_S32_MAX || (int64_t)s->dst.y1 + value > DRM_S32_MAX) { return -EINVAL; }
            s->dst.y2 = s->dst.y1 + (int32_t)value;
            return 0;
        }

        if (prop == config->prop_zpos) {
            s->zpos         = (int)value;
            s->zpos_changed = true;
            return 0;
        }
        if (prop == config->prop_alpha) {
            s->alpha = (unsigned int)value;
            return 0;
        }
        if (prop == config->prop_plane_type) { return 0; /* immutable */ }
    } else if (obj->type == DRM_MODE_OBJECT_CONNECTOR) {
        struct drm_connector       *connector = container_of(obj, struct drm_connector, base);
        struct drm_connector_state *s         = drm_atomic_get_connector_state(state, connector);

        if (s == NULL) { return -ENOMEM; }

        if (prop == config->prop_crtc_id) {
            struct drm_crtc        *old_crtc = s->crtc;
            struct drm_mode_object *target =
                (value != 0) ? drm_mode_object_find(state->dev, file_priv, (uint32_t)value, DRM_MODE_OBJECT_CRTC) : NULL;

            if (value != 0 && target == NULL) { return -ENOENT; }

            s->crtc = (target != NULL) ? container_of(target, struct drm_crtc, base) : NULL;
            if (target != NULL) { drm_mode_object_put(target); }

            s->crtc_changed = true;

            if (old_crtc != NULL) {
                int ret = drm_atomic_mark_crtc(state, old_crtc, true);
                if (ret != 0) { return ret; }
            }
            if (s->crtc != NULL && s->crtc != old_crtc) { return drm_atomic_mark_crtc(state, s->crtc, true); }
            return 0;
        }
    }

    return -EINVAL;
}

/*
 * DRM_IOCTL_MODE_ATOMIC.  Everything is copied in before anything is
 * applied, so a client cannot change the request underneath us while we are
 * validating it.
 */
int drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_atomic  *atomic = (struct drm_mode_atomic *)data;
    struct drm_atomic_state *state;
    uint32_t                *objs        = NULL;
    uint32_t                *count_props = NULL;
    uint32_t                *props       = NULL;
    uint64_t                *prop_values = NULL;
    uint32_t                 total_props = 0;
    int                      ret         = 0;
    uint32_t                 i;

    if (dev == NULL || atomic == NULL || file_priv == NULL || atomic->count_objs > 256 || atomic->reserved != 0) {
        return -EINVAL;
    }
    if ((dev->driver->driver_features & DRIVER_ATOMIC) == 0) { return -EOPNOTSUPP; }
    if (!file_priv->atomic) { return -EINVAL; } /* must ask for atomic first */
    if ((atomic->flags & ~DRM_MODE_ATOMIC_FLAGS) != 0) { return -EINVAL; }
    if ((atomic->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0 && (atomic->flags & DRM_MODE_PAGE_FLIP_EVENT) != 0) {
        return -EINVAL; /* nothing is committed, so nothing can complete */
    }
    if ((atomic->flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0) { return -EINVAL; }

    state = drm_atomic_state_alloc(dev);
    if (state == NULL) { return -ENOMEM; }

    if ((atomic->flags & DRM_MODE_ATOMIC_ALLOW_MODESET) != 0) { state->allow_modeset = 1; }
    state->file_priv       = file_priv;
    state->user_data       = atomic->user_data;
    state->page_flip_event = ((atomic->flags & DRM_MODE_PAGE_FLIP_EVENT) != 0);

    if (atomic->count_objs != 0) {
        if (atomic->objs_ptr == 0 || atomic->count_props_ptr == 0) {
            ret = -EFAULT;
            goto out;
        }

        objs        = malloc((size_t)atomic->count_objs * sizeof(*objs));
        count_props = malloc((size_t)atomic->count_objs * sizeof(*count_props));
        if (objs == NULL || count_props == NULL) {
            ret = -ENOMEM;
            goto out;
        }

        if (copy_from_user(objs, (const void *)(uintptr_t)atomic->objs_ptr, (size_t)atomic->count_objs * sizeof(*objs)) != 0
            || copy_from_user(count_props, (const void *)(uintptr_t)atomic->count_props_ptr,
                              (size_t)atomic->count_objs * sizeof(*count_props)) != 0) {
            ret = -EFAULT;
            goto out;
        }

        for (i = 0; i < atomic->count_objs; i++) {
            if (count_props[i] > 4096 - total_props) {
                ret = -E2BIG;
                goto out;
            }
            total_props += count_props[i];
        }
    }

    if (total_props != 0) {
        if (atomic->props_ptr == 0 || atomic->prop_values_ptr == 0) {
            ret = -EFAULT;
            goto out;
        }

        props       = malloc((size_t)total_props * sizeof(*props));
        prop_values = malloc((size_t)total_props * sizeof(*prop_values));
        if (props == NULL || prop_values == NULL) {
            ret = -ENOMEM;
            goto out;
        }

        if (copy_from_user(props, (const void *)(uintptr_t)atomic->props_ptr, (size_t)total_props * sizeof(*props)) != 0
            || copy_from_user(prop_values, (const void *)(uintptr_t)atomic->prop_values_ptr,
                              (size_t)total_props * sizeof(*prop_values)) != 0) {
            ret = -EFAULT;
            goto out;
        }
    }

    {
        uint32_t prop_offset = 0;

        for (i = 0; i < atomic->count_objs; i++) {
            uint32_t                obj_id    = objs[i];
            uint32_t                obj_count = count_props[i];
            struct drm_mode_object *obj       = drm_mode_object_find(dev, file_priv, obj_id, DRM_MODE_OBJECT_ANY);
            uint32_t                j;

            if (obj == NULL) {
                ret = -ENOENT;
                break;
            }

            /* Only the three things a commit can describe. */
            if (obj->type != DRM_MODE_OBJECT_CRTC && obj->type != DRM_MODE_OBJECT_PLANE
                && obj->type != DRM_MODE_OBJECT_CONNECTOR) {
                drm_mode_object_put(obj);
                ret = -EINVAL;
                break;
            }

            for (j = 0; j < obj_count; j++) {
                struct drm_property *prop = drm_property_find(dev, file_priv, props[prop_offset + j]);

                if (prop == NULL) {
                    ret = -ENOENT;
                    break;
                }

                ret = drm_atomic_validate_property(dev, obj, prop, prop_values[prop_offset + j]);
                if (ret == 0) {
                    ret = drm_atomic_set_uapi_property(state, file_priv, obj, prop, prop_values[prop_offset + j]);
                }

                drm_mode_object_put(&prop->base);
                if (ret != 0) { break; }
            }

            drm_mode_object_put(obj);
            prop_offset += obj_count;
            if (ret != 0) { break; }
        }
    }

    if (ret < 0) { goto out; }

    if ((atomic->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0) {
        ret = drm_atomic_check_only(state);
        drm_atomic_state_free(state);
        state = NULL;
        goto out;
    }

    ret = ((atomic->flags & DRM_MODE_ATOMIC_NONBLOCK) != 0) ? drm_atomic_nonblocking_commit(state) : drm_atomic_commit(state);
    if (ret == 0) { state = NULL; } /* ownership moved into the commit */

    /* Once it is on screen, remember it, so GETPROPERTY reports what the
     * client actually set. */
    if (ret == 0) {
        uint32_t offset = 0;

        for (i = 0; i < atomic->count_objs; i++) {
            struct drm_mode_object *obj = drm_mode_object_find(dev, file_priv, objs[i], DRM_MODE_OBJECT_ANY);

            if (obj == NULL) {
                offset += count_props[i];
                continue;
            }

            for (uint32_t j = 0; j < count_props[i]; j++) {
                struct drm_property *prop = drm_property_find(dev, file_priv, props[offset + j]);

                if (prop != NULL) {
                    drm_object_property_set_value(obj, prop, prop_values[offset + j]);
                    drm_mode_object_put(&prop->base);
                }
            }

            drm_mode_object_put(obj);
            offset += count_props[i];
        }
    }

out:
    if (state != NULL) { drm_atomic_state_free(state); }
    free(objs);
    free(count_props);
    free(props);
    free(prop_values);
    return ret;
}

/*
 * DRM_IOCTL_MODE_PAGE_FLIP: swap the CRTC's framebuffer at the next vblank.
 * The event is built before the driver is called but only armed afterwards,
 * so a flip the hardware refuses does not leave a completion that never
 * comes.
 */
int drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc_page_flip  *page_flip = (struct drm_mode_crtc_page_flip *)data;
    struct drm_crtc                 *crtc;
    struct drm_framebuffer          *fb;
    struct drm_pending_vblank_event *e = NULL;
    struct drm_mode_object          *crtc_obj;
    struct drm_crtc_helper_funcs    *helpers;
    int                              ret = 0;

    if (dev == NULL || page_flip == NULL) { return -EINVAL; }
    if ((page_flip->flags & ~(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC)) != 0) { return -EINVAL; }

    crtc_obj = drm_mode_object_find(dev, file_priv, page_flip->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (crtc_obj == NULL) {
        DRM_ERROR("Page flip: CRTC %u not found\n", page_flip->crtc_id);
        return -ENOENT;
    }

    fb = drm_framebuffer_lookup(dev, file_priv, page_flip->fb_id);
    if (fb == NULL) {
        drm_mode_object_put(crtc_obj);
        DRM_ERROR("Page flip: FB %u not found\n", page_flip->fb_id);
        return -ENOENT;
    }
    crtc = container_of(crtc_obj, struct drm_crtc, base);

    if (!crtc->enabled) {
        drm_mode_object_put(&crtc->base);
        return -EINVAL;
    }
    if ((page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0 && !dev->mode_config.async_page_flip) {
        drm_mode_object_put(&crtc->base);
        return -EINVAL;
    }

    /* A flip cannot change the size of what is scanned out. */
    if (crtc->enabled && crtc->mode.hdisplay > 0 && crtc->mode.vdisplay > 0) {
        if (fb->width != (unsigned int)crtc->mode.hdisplay || fb->height != (unsigned int)crtc->mode.vdisplay) {
            DRM_ERROR("Page flip: FB %ux%u does not match mode %ux%u\n", fb->width, fb->height, crtc->mode.hdisplay,
                      crtc->mode.vdisplay);
            drm_mode_object_put(&crtc->base);
            return -EINVAL;
        }
    }

    spin_lock(&crtc->commit_lock);
    if (crtc->page_flip_pending) {
        spin_unlock(&crtc->commit_lock);
        drm_mode_object_put(&crtc->base);
        return -EBUSY; /* one flip at a time per CRTC */
    }
    crtc->page_flip_pending = true;
    crtc->page_flip_target  = 0;
    spin_unlock(&crtc->commit_lock);

    if ((page_flip->flags & DRM_MODE_PAGE_FLIP_EVENT) != 0) {
        e = malloc(sizeof(*e));
        if (e == NULL) {
            spin_lock(&crtc->commit_lock);
            crtc->page_flip_pending = false;
            spin_unlock(&crtc->commit_lock);
            drm_mode_object_put(&crtc->base);
            return -ENOMEM;
        }
        memset(e, 0, sizeof(*e));

        e->dev               = dev;
        e->file_priv         = file_priv;
        e->crtc              = crtc;
        e->pipe              = crtc->index;
        e->event.base.type   = DRM_EVENT_FLIP_COMPLETE;
        e->event.base.length = sizeof(e->event);
        e->event.user_data   = page_flip->user_data;
        e->event.crtc_id     = crtc->base.id;
        e->destroy           = NULL;
        e->next              = NULL;
    }

    if ((page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) == 0) {
        ret = drm_crtc_vblank_get(crtc);
        if (ret != 0) { goto err_flip; }

        spin_lock(&crtc->commit_lock);
        crtc->page_flip_target = (uint64_t)drm_crtc_vblank_count(crtc) + 1;
        spin_unlock(&crtc->commit_lock);

        if (e != NULL) { e->sequence = crtc->page_flip_target; }
    }

    /* Hardware first: the software state only changes once it accepted it. */
    helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;
    if (helpers == NULL || helpers->page_flip == NULL) {
        ret = -ENOSYS;
        if ((page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) == 0) { drm_crtc_vblank_put(crtc); }
        goto err_flip;
    }

    ret = helpers->page_flip(crtc, fb, e, page_flip->flags);
    if (ret != 0) {
        if ((page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) == 0) { drm_crtc_vblank_put(crtc); }
        goto err_flip;
    }

    if ((page_flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0) {
        spin_lock(&crtc->commit_lock);
        crtc->page_flip_pending = false;
        spin_unlock(&crtc->commit_lock);
        if (e != NULL) { drm_crtc_send_vblank_event(crtc, e); }
    } else if (e != NULL) {
        drm_crtc_arm_vblank_event(crtc, e);
    }

    drm_mode_object_put(&crtc->base);

    DRM_DEBUG_KMS("Page flip: CRTC %u -> FB %u (flags=0x%x)\n", page_flip->crtc_id, page_flip->fb_id, page_flip->flags);

    return ret;

err_flip:
    spin_lock(&crtc->commit_lock);
    crtc->page_flip_pending = false;
    crtc->page_flip_target  = 0;
    spin_unlock(&crtc->commit_lock);
    free(e);
    drm_mode_object_put(&crtc->base);
    return (ret != 0) ? ret : -EINVAL;
}

/*
 * Both cursor ioctls share this: either install a new cursor image (with a
 * hotspot inside it), or move the existing one, or both.
 */
static int drm_mode_cursor_common(struct drm_device *dev, struct drm_file *file_priv, struct drm_mode_cursor *cursor, int32_t hot_x,
                                  int32_t hot_y)
{
    struct drm_mode_object       *base;
    struct drm_crtc              *crtc;
    struct drm_crtc_helper_funcs *helpers;
    struct drm_gem_object        *new_obj = NULL, *old_obj = NULL;
    int                           ret = 0;

    if (dev == NULL || file_priv == NULL || cursor == NULL || cursor->flags == 0
        || (cursor->flags & ~(DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE)) != 0) {
        return -EINVAL;
    }

    base = drm_mode_object_find(dev, file_priv, cursor->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (base == NULL) { return -ENOENT; }

    crtc    = container_of(base, struct drm_crtc, base);
    helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;

    if ((cursor->flags & DRM_MODE_CURSOR_BO) != 0) {
        if (helpers == NULL || helpers->cursor_set == NULL) {
            ret = -ENOSYS;
            goto out;
        }

        if (cursor->handle != 0) {
            /* A cursor is ARGB8888, so the buffer has to be big enough for
             * four bytes a pixel, and the hotspot has to be inside it. */
            if (cursor->width == 0 || cursor->height == 0 || hot_x < 0 || hot_y < 0
                || (uint32_t)hot_x >= cursor->width || (uint32_t)hot_y >= cursor->height) {
                ret = -EINVAL;
                goto out;
            }

            new_obj = drm_gem_object_lookup(file_priv, cursor->handle);
            if (new_obj == NULL) {
                ret = -ENOENT;
                goto out;
            }
            if (new_obj->size < (size_t)cursor->width * cursor->height * 4) {
                ret = -EINVAL;
                goto out;
            }
        }

        ret = helpers->cursor_set(crtc, new_obj, cursor->width, cursor->height, hot_x, hot_y);
        if (ret != 0) { goto out; }

        /* Swap under the CRTC's own lock and drop the reference the old
         * image was holding. */
        spin_lock(&crtc->spinlock);
        old_obj            = crtc->cursor_obj;
        crtc->cursor_obj   = new_obj;
        crtc->cursor_hot_x = hot_x;
        crtc->cursor_hot_y = hot_y;
        new_obj            = NULL;
        spin_unlock(&crtc->spinlock);

        if (old_obj != NULL) { drm_gem_object_put(old_obj); }
    }

    if ((cursor->flags & DRM_MODE_CURSOR_MOVE) != 0) {
        if (helpers == NULL || helpers->cursor_move == NULL) {
            ret = -ENOSYS;
            goto out;
        }

        ret = helpers->cursor_move(crtc, cursor->x, cursor->y);
        if (ret != 0) { goto out; }

        crtc->x = cursor->x;
        crtc->y = cursor->y;
    }

out:
    if (new_obj != NULL) { drm_gem_object_put(new_obj); }
    drm_mode_object_put(base);
    return ret;
}

int drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    return drm_mode_cursor_common(dev, file_priv, (struct drm_mode_cursor *)data, 0, 0);
}

int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_cursor2 *cursor2 = (struct drm_mode_cursor2 *)data;

    return drm_mode_cursor_common(dev, file_priv, &cursor2->req, cursor2->hot_x, cursor2->hot_y);
}
