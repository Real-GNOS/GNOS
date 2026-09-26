/*
 * drm_mode_object.c - the id table behind every KMS object. (GPLv2)
 *
 * User space never sees a kernel pointer.  It sees a 32-bit id, and this
 * file is where ids come from, what they resolve to, and when the object
 * behind one may be freed.  Every CRTC, connector, encoder, plane,
 * framebuffer, property and mode starts life as a drm_mode_object, so this
 * is the shared substrate of the whole KMS interface.
 *
 * Two tables are consulted.  The device-wide one holds everything the
 * driver created; the per-file one holds things a particular client was
 * given a handle for.  A lookup tries the device table first and falls back
 * to the caller's own -- which is what stops one client from reaching an
 * object another one created but never shared.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_port.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Implemented in drm_property.c. */
extern struct drm_property *drm_property_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id);

/* Slots a property set starts with; most objects have fewer than a dozen. */
#define DRM_OBJECT_PROP_INITIAL_CAPACITY 16u

/* --------------------------------------------------------- ids and refcounts */

/*
 * Give @obj an id and publish it.  The object comes back with one
 * reference, owned by whoever asked for it.
 */
int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type)
{
    uint32_t id  = 0;
    int      ret;

    spin_lock(&dev->mode_config.idr_mutex);
    ret = drm_idr_alloc(&dev->mode_config.object_idr, obj, 1, 0, &id);
    spin_unlock(&dev->mode_config.idr_mutex);
    if (ret != 0) { return ret; }

    obj->id         = id;
    obj->type       = type;
    obj->dev        = dev;
    obj->refcount   = 1;
    obj->properties = NULL;
    memset(&obj->ref_lock, 0, sizeof(obj->ref_lock));

    return 0;
}

void drm_mode_object_get(struct drm_mode_object *obj)
{
    if (obj == NULL) { return; }

    spin_lock(&obj->ref_lock);
    obj->refcount++;
    spin_unlock(&obj->ref_lock);
}

/*
 * Drop a reference and say whether it was the last one.  Deciding that
 * under the same lock as the decrement is the point: a separate "is it
 * zero now?" check afterwards can be jumped by another CPU, which is how
 * objects get freed twice.
 */
bool drm_mode_object_put_dec_and_test(struct drm_mode_object *obj)
{
    bool last;

    if (obj == NULL) { return false; }

    spin_lock(&obj->ref_lock);
    last = (--obj->refcount == 0);
    spin_unlock(&obj->ref_lock);

    return last;
}

void drm_mode_object_put(struct drm_mode_object *obj)
{
    (void)drm_mode_object_put_dec_and_test(obj);
}

/*
 * Resolve @id.  @type filters what may come back; DRM_MODE_OBJECT_ANY
 * accepts anything.  On success the object carries an extra reference the
 * caller is responsible for dropping.
 */
struct drm_mode_object *drm_mode_object_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id, uint32_t type)
{
    struct drm_mode_object *obj;

    spin_lock(&dev->mode_config.idr_mutex);
    obj = drm_idr_find(&dev->mode_config.object_idr, id);
    if (obj != NULL && (type == DRM_MODE_OBJECT_ANY || obj->type == type)) {
        drm_mode_object_get(obj);
        spin_unlock(&dev->mode_config.idr_mutex);
        return obj;
    }
    spin_unlock(&dev->mode_config.idr_mutex);

    /* Not ours to share: try what this particular client was given. */
    if (file_priv != NULL) {
        spin_lock(&file_priv->table_lock);
        obj = drm_idr_find(&file_priv->object_idr, id);
        if (obj != NULL && (type == DRM_MODE_OBJECT_ANY || obj->type == type)) {
            drm_mode_object_get(obj);
            spin_unlock(&file_priv->table_lock);
            return obj;
        }
        spin_unlock(&file_priv->table_lock);
    }

    return NULL;
}

/* --------------------------------------------------- per-object properties */

/* Set (or overwrite) @property's value on @obj. */
int drm_object_property_set_value(struct drm_mode_object *obj, struct drm_property *property, uint64_t val)
{
    struct drm_property_set *set;
    uint32_t                 i;

    if (obj == NULL || property == NULL) { return -EINVAL; }
    set = obj->properties;
    if (set == NULL) { return -EINVAL; }

    spin_lock(&set->lock);

    for (i = 0; i < set->count; i++) {
        if (set->ids[i] == property->base.id) {
            set->values[i] = val;
            spin_unlock(&set->lock);
            return 0;
        }
    }

    if (set->count >= set->capacity) {
        uint32_t  wanted = (set->capacity != 0) ? set->capacity * 2u : DRM_OBJECT_PROP_INITIAL_CAPACITY;
        uint32_t *ids    = realloc(set->ids, (size_t)wanted * sizeof(*ids));
        uint64_t *values;

        if (ids == NULL) {
            spin_unlock(&set->lock);
            return -ENOMEM;
        }
        set->ids = ids; /* realloc may have moved it */

        values = realloc(set->values, (size_t)wanted * sizeof(*values));
        if (values == NULL) {
            /* The ids array grew but the values one did not, so the extra
             * room cannot be used: leave the capacity alone and let the
             * next attempt reuse the ids headroom. */
            spin_unlock(&set->lock);
            return -ENOMEM;
        }

        set->values   = values;
        set->capacity = wanted;
    }

    set->ids[set->count]    = property->base.id;
    set->values[set->count] = val;
    set->count++;

    spin_unlock(&set->lock);
    return 0;
}

/* Read @property's value back.  -EINVAL when it is not attached. */
int drm_object_property_get_value(struct drm_mode_object *obj, struct drm_property *property, uint64_t *val_out)
{
    struct drm_property_set *set;
    uint32_t                 i;

    if (obj == NULL || property == NULL || val_out == NULL) { return -EINVAL; }
    set = obj->properties;
    if (set == NULL) { return -EINVAL; }

    spin_lock(&set->lock);
    for (i = 0; i < set->count; i++) {
        if (set->ids[i] == property->base.id) {
            *val_out = set->values[i];
            spin_unlock(&set->lock);
            return 0;
        }
    }
    spin_unlock(&set->lock);

    return -EINVAL;
}

/*
 * Attach @property to @obj, creating the object's property set on first
 * use.  Meant for object construction: the set is not grown in a way that
 * is safe against a concurrent reader walking it.
 */
int drm_object_attach_property(struct drm_mode_object *obj, struct drm_property *property, uint64_t init_val)
{
    if (obj == NULL || property == NULL) { return -EINVAL; }

    if (obj->properties == NULL) {
        struct drm_property_set *set  = malloc(sizeof(*set));
        uint32_t                *ids  = malloc((size_t)DRM_OBJECT_PROP_INITIAL_CAPACITY * sizeof(*ids));
        uint64_t                *vals = malloc((size_t)DRM_OBJECT_PROP_INITIAL_CAPACITY * sizeof(*vals));

        if (set == NULL || ids == NULL || vals == NULL) {
            free(set);
            free(ids);
            free(vals);
            return -ENOMEM;
        }

        memset(set, 0, sizeof(*set));
        set->capacity   = DRM_OBJECT_PROP_INITIAL_CAPACITY;
        set->ids        = ids;
        set->values     = vals;

        obj->properties = set;
    }

    return drm_object_property_set_value(obj, property, init_val);
}

void drm_property_set_init(struct drm_property_set *set)
{
    if (set == NULL) { return; }

    memset(set, 0, sizeof(*set));
}

void drm_property_set_destroy(struct drm_property_set *set)
{
    if (set == NULL) { return; }

    free(set->ids);
    free(set->values);
    memset(set, 0, sizeof(*set));
}

/* --------------------------------------------------------------- ioctls */

/*
 * DRM_IOCTL_MODE_OBJ_GETPROPERTIES.  Called twice in practice: once with
 * count_props == 0 to learn how much space to allocate, then again with
 * the arrays filled in.  We report how many exist and copy no more than
 * the caller has room for.
 */
int drm_mode_obj_getproperties_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_obj_get_properties *req = (struct drm_mode_obj_get_properties *)data;
    struct drm_mode_object             *obj;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, req->obj_id, req->obj_type);
    if (obj == NULL) { return -ENOENT; }

    if (obj->properties != NULL) {
        struct drm_property_set *set     = obj->properties;
        uint32_t                 wanted  = req->count_props;
        uint32_t                 copying = 0;
        uint32_t                *ids     = NULL;
        uint64_t                *values  = NULL;

        spin_lock(&set->lock);
        req->count_props = set->count;
        copying          = (wanted < set->count) ? wanted : set->count;
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
    } else {
        req->count_props = 0;
    }

    drm_mode_object_put(obj);
    return 0;
}

/*
 * DRM_IOCTL_MODE_OBJ_SETPROPERTY.  Refuses atomic properties (those only
 * make sense inside a MODE_ATOMIC commit), immutable ones, values outside
 * a range property's bounds, and properties the object does not carry.
 */
int drm_mode_obj_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_obj_set_property *req  = (struct drm_mode_obj_set_property *)data;
    struct drm_mode_object           *obj;
    struct drm_property              *prop;
    uint64_t                          current_value;
    int                               ret;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    obj = drm_mode_object_find(dev, NULL, req->obj_id, req->obj_type);
    if (obj == NULL) { return -ENOENT; }

    prop = drm_property_find(dev, NULL, req->prop_id);
    if (prop == NULL) {
        drm_mode_object_put(obj);
        return -ENOENT;
    }

    if ((prop->flags & DRM_MODE_PROP_ATOMIC) != 0 || obj->properties == NULL) {
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        return -EINVAL;
    }

    if (drm_object_property_get_value(obj, prop, &current_value) != 0) {
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        return -ENOENT;
    }

    if ((prop->flags & DRM_MODE_PROP_IMMUTABLE) != 0) {
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        return -EINVAL;
    }

    if ((prop->flags & DRM_MODE_PROP_RANGE) != 0 && (req->value < prop->values[0] || req->value > prop->values[1])) {
        drm_mode_object_put(&prop->base);
        drm_mode_object_put(obj);
        return -EINVAL;
    }

    ret = drm_object_property_set_value(obj, prop, req->value);

    drm_mode_object_put(&prop->base);
    drm_mode_object_put(obj);
    return ret;
}

/*
 * DRM_IOCTL_MODE_SETPROPERTY: the pre-atomic spelling, which names a
 * connector instead of an arbitrary object.  Xorg's modesetting driver
 * still uses it whenever atomic is not offered, so translate and share
 * the object-based implementation above.
 */
struct drm_mode_set_property {
    uint64_t value;
    uint32_t prop_id;
    uint32_t connector_id;
};

int drm_mode_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_set_property     *legacy = (struct drm_mode_set_property *)data;
    struct drm_mode_obj_set_property  modern;

    if (dev == NULL || legacy == NULL) { return -EINVAL; }

    memset(&modern, 0, sizeof(modern));
    modern.obj_id   = legacy->connector_id;
    modern.obj_type = DRM_MODE_OBJECT_CONNECTOR;
    modern.prop_id  = legacy->prop_id;
    modern.value    = legacy->value;

    return drm_mode_obj_setproperty_ioctl(dev, &modern, file_priv);
}
