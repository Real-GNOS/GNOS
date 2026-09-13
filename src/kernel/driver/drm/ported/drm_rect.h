/*
 * drm_rect.h - half-open rectangles, the units display hardware thinks in.
 * (GPLv2)
 *
 * Every KMS request eventually reduces to rectangles: a framebuffer's
 * visible window, the CRT controller's active area, the destination a plane
 * is scaled into.  Sides are stored as coordinates, not as position plus
 * size, so that several rectangles share edges without arithmetic drift --
 * and [x1, x2) is half-open, which makes tiling and intersection come out
 * right with no off-by-one anywhere.
 */

#ifndef INCLUDE_DRM_DRM_RECT_H_
#define INCLUDE_DRM_DRM_RECT_H_

#include <stdbool.h>
#include <stdint.h>

struct drm_rect {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

#define DRM_RECT_FMT    "x%d %d %dx%d"
#define DRM_RECT_ARG(r) (r)->x1, (r)->y1, drm_rect_width(r), drm_rect_height(r)

static inline int drm_rect_width(const struct drm_rect *r)
{
    return r->x2 - r->x1;
}

static inline int drm_rect_height(const struct drm_rect *r)
{
    return r->y2 - r->y1;
}

/* Make @r cover [x, x+w) x [y, y+h). */
static inline void drm_rect_init(struct drm_rect *r, int x, int y, int w, int h)
{
    r->x1 = x;
    r->y1 = y;
    r->x2 = x + w;
    r->y2 = y + h;
}

/* Grow @r by @dw columns and @dh rows, keeping its top-left corner put. */
static inline void drm_rect_adjust_size(struct drm_rect *r, int dw, int dh)
{
    r->x2 += dw;
    r->y2 += dh;
}

/* Move @r by (dx, dy), keeping its size. */
static inline void drm_rect_translate(struct drm_rect *r, int dx, int dy)
{
    r->x1 += dx;
    r->y1 += dy;
    r->x2 += dx;
    r->y2 += dy;
}

/* Move @r's top-left corner to (x, y), keeping its size. */
static inline void drm_rect_translate_to(struct drm_rect *r, int x, int y)
{
    r->x2 += x - r->x1;
    r->y2 += y - r->y1;
    r->x1 = x;
    r->y1 = y;
}

/* Cut @r down to what it shares with @clip.  True when anything is left. */
bool drm_rect_intersect(struct drm_rect *r, const struct drm_rect *clip);

/*
 * Clip @dst to @clip and trim @src by the corresponding amount, keeping the
 * source-to-destination scale intact -- used when a scaled plane pokes out
 * of the screen.  True when @dst is still visible afterwards.
 */
bool drm_rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst, const struct drm_rect *clip);

/* True when @r covers a positive area: degenerate rectangles are invisible. */
bool drm_rect_visible(const struct drm_rect *r);

#endif /* INCLUDE_DRM_DRM_RECT_H_ */
