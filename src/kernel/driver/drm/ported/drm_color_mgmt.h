/*
 * drm_color_mgmt.h - colour management, as far as the UAPI goes. (GPLv2)
 *
 * Between the framebuffer and the panel sit two lookup tables (degamma on
 * the way in, gamma on the way out) and optionally a colour transformation
 * matrix.  This header only describes how those reach the kernel: as blob
 * properties whose payloads are the structs below.  They are user-space
 * facing types, so their layout is part of the interface.
 */

#ifndef INCLUDE_DRM_DRM_COLOR_MGMT_H_
#define INCLUDE_DRM_DRM_COLOR_MGMT_H_

#include "drm.h"

/* Which table a blob property holds. */
#define DRM_MODE_LUT_GAMMA   (1 << 0)
#define DRM_MODE_LUT_DEGAMMA (1 << 1)

/* One entry of a lookup table: 16 bits per channel, which is what every
 * panel's own table expects. */
struct drm_color_lut {
    __u16 red;
    __u16 green;
    __u16 blue;
    __u16 reserved;
};

/* A 3x3 colour transformation matrix, signed 4.32 fixed point. */
struct drm_color_ctm {
    __s64 matrix[9];
};

/* How a piecewise lookup table is split into segments. */
struct drm_color_lut_range {
    __u16 ramp_size;  /* samples covered by one step       */
    __u16 lut_size;   /* entries belonging to this segment */
    __u32 properties; /* segment flags                     */
    __u32 start;      /* first input value, inclusive      */
    __u32 end;        /* last input value, inclusive       */
};

#endif /* INCLUDE_DRM_DRM_COLOR_MGMT_H_ */
