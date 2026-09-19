/*
 * drm_property.c - named settings on display objects. (GPLv2)
 *
 * A property is a name carrying a value: "which CRTC does this connector
 * use", "how bright is the panel", "what gamma table".  Making them
 * first-class objects -- rather than one ioctl each -- is what lets a
 * client enumerate what a device supports instead of guessing.  Four kinds
 * exist: a plain number, a number within a range, one of a list of named
 * values (enum), or any combination of those values (bitmask).  Anything
 * bulkier (a gamma table, a list of modes) goes in a blob, which is just a
 * refcounted byte string with an id.
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

/* From drm_mode_object.c. */
extern int  drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);
extern bool drm_mode_object_put_dec_and_test(struct drm_mode_object *obj);

/* Defined below; needed by the create paths so they can unwind. */
void drm_property_destroy(struct drm_device *dev, struct drm_property *property);

/* ------------------------------------------------------------ enum entries */

/* One named value, linked into the property and mirrored into values[i]. */
static int drm_property_add_enum(struct drm_property *prop, int index, uint64_t value, const char *name)
{
    struct drm_property_enum *entry = malloc(sizeof(*entry));

    if (entry == NULL) { return -ENOMEM; }
    memset(entry, 0, sizeof(*entry));

    entry->value = value;
    strncpy(entry->name, name, DRM_PROP_NAME_LEN - 1);
    entry->name[DRM_PROP_NAME_LEN - 1] = '\0';

    ilist_insert_after(&prop->enum_list, &entry->head);
    prop->values[index] = value;

    return 0;
}

static void drm_property_free_enum_list(struct drm_property *prop)
{
    ilist_node_t *node = prop->enum_list.next;

    while (node != NULL && node != &prop->enum_list) {
        /* Unlinking clears this node's pointers, so hold the next one. */
        ilist_node_t             *next  = node->next;
        struct drm_property_enum *entry = container_of(node, struct drm_property_enum, head);

        ilist_remove(node);
        free(entry);
        node = next;
    }
}

/* ---------------------------------------------------------------- creation */

/*
 * The common part of every kind: name it, give it an id, allocate @num_values
 * value slots and put it on the device's property list.
 */
struct drm_property *drm_property_create(struct drm_device *dev, uint32_t flags, const char *name, int num_values)
{
    struct drm_property *prop;

    if (dev == NULL || name == NULL || num_values < 0) { return NULL; }

    prop = malloc(sizeof(*prop));
    if (prop == NULL) { return NULL; }
    memset(prop, 0, sizeof(*prop));

    if (drm_mode_object_idr_alloc(dev, &prop->base, DRM_MODE_OBJECT_PROPERTY) != 0) {
        free(prop);
        return NULL;
    }

    strncpy(prop->name, name, DRM_PROP_NAME_LEN - 1);
    prop->name[DRM_PROP_NAME_LEN - 1] = '\0';

    prop->flags      = flags;
    prop->num_values = (uint32_t)num_values;

    if (num_values > 0) {
        prop->values = malloc((size_t)num_values * sizeof(uint64_t));
        if (prop->values == NULL) {
            /* Undo the publication: nobody can have seen the id yet. */
            spin_lock(&dev->mode_config.idr_mutex);
            drm_idr_remove(&dev->mode_config.object_idr, prop->base.id);
            spin_unlock(&dev->mode_config.idr_mutex);
            free(prop);
            return NULL;
        }
        memset(prop->values, 0, (size_t)num_values * sizeof(uint64_t));
    }

    ilist_init(&prop->enum_list);

    spin_lock(&dev->mode_config.mutex);
    ilist_insert_after(&dev->mode_config.property_list, &prop->dev_head);
    spin_unlock(&dev->mode_config.mutex);

    prop->dev = dev;
    return prop;
}

struct drm_property *drm_property_create_range(struct drm_device *dev, uint32_t flags, const char *name, uint64_t min, uint64_t max)
{
    struct drm_property *prop;

    if (dev == NULL || name == NULL) { return NULL; }

    prop = drm_property_create(dev, DRM_MODE_PROP_RANGE | flags, name, 2);
    if (prop == NULL) { return NULL; }

    prop->values[0] = min;
    prop->values[1] = max;
    return prop;
}

struct drm_property *drm_property_create_enum(struct drm_device *dev, uint32_t flags, const char *name,
                                              const struct drm_mode_property_enum *enums, int num_enums)
{
    struct drm_property *prop;
    int                  i;

    if (dev == NULL || name == NULL) { return NULL; }
    if (num_enums < 0) { return NULL; }
    if (num_enums > 0 && enums == NULL) { return NULL; }

    prop = drm_property_create(dev, DRM_MODE_PROP_ENUM | flags, name, num_enums);
    if (prop == NULL) { return NULL; }

    for (i = 0; i < num_enums; i++) {
        if (drm_property_add_enum(prop, i, enums[i].value, enums[i].name) != 0) {
            drm_property_destroy(dev, prop);
            return NULL;
        }
    }
    return prop;
}

/*
 * A bitmask property is an enum property restricted to the values the
 * driver actually supports: @supported_bits says which entries of @enums
 * apply, and the rest are left out entirely rather than offered and
 * refused.
 */
struct drm_property *drm_property_create_bitmask(struct drm_device *dev, uint32_t flags, const char *name,
                                                 const struct drm_mode_property_enum *enums, int num_enums,
                                                 uint32_t supported_bits)
{
    struct drm_property *prop;
    int                  i, kept;

    if (dev == NULL || name == NULL) { return NULL; }
    if (num_enums < 0) { return NULL; }
    if (num_enums > 0 && enums == NULL) { return NULL; }

    kept = 0;
    for (i = 0; i < num_enums; i++) {
        if (i < 32 && (supported_bits & (1U << i))) { kept++; }
    }

    prop = drm_property_create(dev, DRM_MODE_PROP_BITMASK | flags, name, kept);
    if (prop == NULL) { return NULL; }

    kept = 0;
    for (i = 0; i < num_enums; i++) {
        if (i >= 32 || (supported_bits & (1U << i)) == 0) { continue; }
        if (drm_property_add_enum(prop, kept, enums[i].value, enums[i].name) != 0) {
            drm_property_destroy(dev, prop);
            return NULL;
        }
        kept++;
    }
    return prop;
}

/* ------------------------------------------------------------------- blobs */

/* A refcounted byte string with an id, for payloads too big to be a value. */
struct drm_property_blob *drm_property_create_blob(struct drm_device *dev, const void *data, size_t length)
{
    struct drm_property_blob *blob;
    void                     *copy = NULL;

    if (dev == NULL) { return NULL; }
    if (length > 0 && data == NULL) { return NULL; }

    blob = malloc(sizeof(*blob));
    if (blob == NULL) { return NULL; }
    memset(blob, 0, sizeof(*blob));

    if (length > 0) {
        copy = malloc(length);
        if (copy == NULL) {
            free(blob);
            return NULL;
        }
        memcpy(copy, data, length);
    }

    if (drm_mode_object_idr_alloc(dev, &blob->base, DRM_MODE_OBJECT_BLOB) != 0) {
        free(copy);
        free(blob);
        return NULL;
    }

    blob->data   = copy;
    blob->length = length;

    spin_lock(&dev->mode_config.blob_lock);
    ilist_insert_after(&dev->mode_config.property_blob_list, &blob->head_global);
    spin_unlock(&dev->mode_config.blob_lock);

    return blob;
}

void drm_property_blob_get(struct drm_property_blob *blob)
{
    if (blob == NULL) { return; }

    drm_mode_object_get(&blob->base);
}

/*
 * Last reference out: forget the id, leave the device's blob list, free
 * the payload.  The decrement decides the winner under the object's own
 * lock, so exactly one caller gets here.
 */
void drm_property_blob_put(struct drm_property_blob *blob)
{
    struct drm_device *dev;

    if (blob == NULL) { return; }
    if (!drm_mode_object_put_dec_and_test(&blob->base)) { return; }

    dev = blob->base.dev;

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, blob->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    spin_lock(&dev->mode_config.blob_lock);
    ilist_remove(&blob->head_global);
    spin_unlock(&dev->mode_config.blob_lock);

    free(blob->data);
    free(blob);
}

struct drm_property_blob *drm_property_lookup_blob(struct drm_device *dev, uint32_t id)
{
    struct drm_mode_object *obj;

    if (dev == NULL) { return NULL; }

    obj = drm_mode_object_find(dev, NULL, id, DRM_MODE_OBJECT_BLOB);
    if (obj == NULL) { return NULL; }

    return container_of(obj, struct drm_property_blob, base);
}

/* ------------------------------------------------------- destruction, lookup */

void drm_property_destroy(struct drm_device *dev, struct drm_property *property)
{
    if (dev == NULL || property == NULL) { return; }

    spin_lock(&dev->mode_config.mutex);
    ilist_remove(&property->dev_head);
    spin_unlock(&dev->mode_config.mutex);

    drm_property_free_enum_list(property);

    free(property->values);
    property->values = NULL;

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, property->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    free(property);
}

void drm_property_destroy_user(struct drm_device *dev, struct drm_property *property)
{
    drm_property_destroy(dev, property);
}

struct drm_property *drm_property_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id)
{
    struct drm_mode_object *obj;

    if (dev == NULL) { return NULL; }

    obj = drm_mode_object_find(dev, file_priv, id, DRM_MODE_OBJECT_PROPERTY);
    if (obj == NULL) { return NULL; }

    return container_of(obj, struct drm_property, base);
}

/* ----------------------------------------------------------------- ioctls */

/*
 * DRM_IOCTL_MODE_GETPROPERTY.  Copies at most what the caller has room
 * for and always reports the full counts, which is how a client sizes its
 * buffers: ask with zero, allocate, ask again.
 */
int drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_property *req = (struct drm_mode_get_property *)data;
    struct drm_property          *prop;
    ilist_node_t                 *node;
    uint32_t                      values_wanted, enums_wanted, enum_count = 0;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    prop = drm_property_find(dev, NULL, req->prop_id);
    if (prop == NULL) { return -ENOENT; }

    values_wanted = req->count_values;
    enums_wanted  = req->count_enum_blobs;

    strncpy(req->name, prop->name, DRM_PROP_NAME_LEN - 1);
    req->name[DRM_PROP_NAME_LEN - 1] = '\0';
    req->flags                       = prop->flags;
    req->count_values                = prop->num_values;

    for (node = prop->enum_list.next; node != NULL && node != &prop->enum_list; node = node->next) { enum_count++; }
    req->count_enum_blobs = enum_count;

    if (values_wanted != 0 && prop->num_values != 0) {
        uint32_t count = (values_wanted < prop->num_values) ? values_wanted : prop->num_values;

        if (req->values_ptr == 0
            || copy_to_user((void *)(uintptr_t)req->values_ptr, prop->values, (size_t)count * sizeof(*prop->values)) != 0) {
            drm_mode_object_put(&prop->base);
            return -EFAULT;
        }
    }

    if (enums_wanted != 0 && enum_count != 0) {
        uint32_t                       count   = (enums_wanted < enum_count) ? enums_wanted : enum_count;
        struct drm_mode_property_enum *entries = malloc((size_t)count * sizeof(*entries));

        if (entries == NULL) {
            drm_mode_object_put(&prop->base);
            return -ENOMEM;
        }

        node = prop->enum_list.next;
        for (uint32_t i = 0; i < count; i++, node = node->next) {
            struct drm_property_enum *entry = container_of(node, struct drm_property_enum, head);

            entries[i].value = entry->value;
            memcpy(entries[i].name, entry->name, sizeof(entries[i].name));
        }

        if (req->enum_blob_ptr == 0
            || copy_to_user((void *)(uintptr_t)req->enum_blob_ptr, entries, (size_t)count * sizeof(*entries)) != 0) {
            free(entries);
            drm_mode_object_put(&prop->base);
            return -EFAULT;
        }
        free(entries);
    }

    drm_mode_object_put(&prop->base);
    return 0;
}

/*
 * DRM_IOCTL_MODE_GETPROPBLOB.  Same two-pass shape as the property query:
 * the caller learns the length on the first call and supplies a buffer on
 * the second.
 */
int drm_mode_getpropblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_blob *req = (struct drm_mode_get_blob *)data;
    struct drm_property_blob *blob;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    blob = drm_property_lookup_blob(dev, req->blob_id);
    if (blob == NULL) {
        req->length = 0;
        return -ENOENT;
    }

    req->length = (uint32_t)blob->length;

    if (req->data != 0 && blob->length > 0 && req->length >= blob->length) {
        if (copy_to_user((void *)(uintptr_t)req->data, blob->data, blob->length) != 0) {
            drm_property_blob_put(blob);
            return -EFAULT;
        }
    }

    drm_property_blob_put(blob);
    return 0;
}

/*
 * DRM_IOCTL_MODE_CREATEPROPBLOB.  The payload arrives through the user
 * pointer in the request; the kernel takes its own copy and hands back an
 * id.  Length zero is legal (an empty blob) and still allocates an id.
 */
int drm_mode_createpropblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_create_blob *req = (struct drm_mode_create_blob *)data;
    struct drm_property_blob    *blob;
    void                        *payload = NULL;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }
    if (req->length > 128 * 1024) { return -E2BIG; }

    if (req->length > 0) {
        payload = malloc(req->length);
        if (payload == NULL) { return -ENOMEM; }
        if (copy_from_user(payload, (const void *)(uintptr_t)req->data,
                           req->length) != 0) {
            free(payload);
            return -EFAULT;
        }
    }

    blob = drm_property_create_blob(dev, payload, req->length);
    free(payload);
    if (blob == NULL) { return -ENOMEM; }

    req->blob_id = blob->base.id;
    return 0;
}

/*
 * DRM_IOCTL_MODE_DESTROYPROPBLOB.  Drops the creator's reference; the blob
 * goes away once nothing else holds it.  The lookup takes a reference of
 * its own (drm_mode_object_find), so two puts: one for the lookup, one for
 * the reference drm_property_create_blob handed the creator.
 */
int drm_mode_destroypropblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_destroy_blob *req = (struct drm_mode_destroy_blob *)data;
    struct drm_property_blob     *blob;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    blob = drm_property_lookup_blob(dev, req->blob_id);
    if (blob == NULL) { return -ENOENT; }

    drm_property_blob_put(blob);      /* our lookup reference */
    drm_property_blob_put(blob);      /* the creator's reference */
    return 0;
}
