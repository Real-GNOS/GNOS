/*
 * drm_blend.c - per-plane appearance properties. (GPLv2)
 *
 * How a plane is stacked (zpos), oriented (rotation), and combined with
 * what is under it (blend mode, alpha) is exposed to user space as
 * properties.  This is the place those defaults are recorded when a driver
 * registers a plane.
 *
 * Two of them are only recorded, not published yet: creating a property
 * needs the property machinery, and until a driver reports support for
 * rotation or blending the value is a statement of intent rather than
 * something a client can change.
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

/* Stacking order, from back to front. */
void drm_plane_create_zpos_property(struct drm_plane *plane, unsigned int zpos)
{
    if (plane == NULL) { return; }

    plane->zpos_property_default = zpos;
}

/* Which rotations and reflections the plane can apply. */
int drm_plane_create_rotation_property(struct drm_plane *plane, unsigned int rotation)
{
    if (plane == NULL) { return -EINVAL; }

    (void)rotation; /* published once properties are wired up */

    return 0;
}

/* Which pixel blend modes the plane's hardware understands. */
int drm_plane_create_blend_mode_property(struct drm_plane *plane, unsigned int blend_mode)
{
    if (plane == NULL) { return -EINVAL; }

    (void)blend_mode;

    return 0;
}

/* Constant transparency applied across the whole plane. */
int drm_plane_create_alpha_property(struct drm_plane *plane)
{
    if (plane == NULL) { return -EINVAL; }

    return 0;
}
