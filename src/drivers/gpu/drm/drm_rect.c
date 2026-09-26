/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_rect.c - half-open rectangles, the units display hardware thinks in.
 * (GPLv2)
 *
 * Nothing here allocates or loops; see drm_rect.h for the contracts.
 */

#include <stdbool.h>
#include <stdint.h>

#include "drm_rect.h"

bool drm_rect_visible(const struct drm_rect *r)
{
    return (r->x2 > r->x1) && (r->y2 > r->y1);
}

bool drm_rect_intersect(struct drm_rect *r, const struct drm_rect *clip)
{
    int32_t left   = (r->x1 > clip->x1) ? r->x1 : clip->x1;
    int32_t top    = (r->y1 > clip->y1) ? r->y1 : clip->y1;
    int32_t right  = (r->x2 < clip->x2) ? r->x2 : clip->x2;
    int32_t bottom = (r->y2 < clip->y2) ? r->y2 : clip->y2;

    r->x1 = left;
    r->y1 = top;
    r->x2 = right;
    r->y2 = bottom;

    /* Empty is fine as a result, it just is not drawable: an intersection
     * that misses entirely leaves the caller with a degenerate rectangle. */
    return drm_rect_visible(r);
}

bool drm_rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst, const struct drm_rect *clip)
{
    int64_t overshoot;

    /* Nothing to scale against, so nothing can be trimmed consistently. */
    if (drm_rect_width(dst) == 0 || drm_rect_height(dst) == 0) { return drm_rect_visible(dst); }

    /* Each side is handled on its own: how far @dst pokes past @clip is
     * converted into source units through the width ratio, so whatever ends
     * up clipped describes the same picture, only smaller. */
    overshoot = (int64_t)clip->x1 - (int64_t)dst->x1;
    if (overshoot > 0) {
        int64_t shift = overshoot * (int64_t)drm_rect_width(src) / (int64_t)drm_rect_width(dst);
        if (shift < 0) { shift = 0; }
        src->x1 = (int32_t)((int64_t)src->x1 + shift);
        dst->x1 = clip->x1;
    }

    overshoot = (int64_t)clip->x2 - (int64_t)dst->x2;
    if (overshoot < 0) {
        int64_t shift = overshoot * (int64_t)drm_rect_width(src) / (int64_t)drm_rect_width(dst);
        if (shift > 0) { shift = 0; }
        src->x2 = (int32_t)((int64_t)src->x2 + shift);
        dst->x2 = clip->x2;
    }

    overshoot = (int64_t)clip->y1 - (int64_t)dst->y1;
    if (overshoot > 0) {
        int64_t shift = overshoot * (int64_t)drm_rect_height(src) / (int64_t)drm_rect_height(dst);
        if (shift < 0) { shift = 0; }
        src->y1 = (int32_t)((int64_t)src->y1 + shift);
        dst->y1 = clip->y1;
    }

    overshoot = (int64_t)clip->y2 - (int64_t)dst->y2;
    if (overshoot < 0) {
        int64_t shift = overshoot * (int64_t)drm_rect_height(src) / (int64_t)drm_rect_height(dst);
        if (shift > 0) { shift = 0; }
        src->y2 = (int32_t)((int64_t)src->y2 + shift);
        dst->y2 = clip->y2;
    }

    return drm_rect_visible(dst);
}
