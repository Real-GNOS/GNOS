/* SPDX-License-Identifier: GPL-2.0 */
/*
 * simpledrm.h - the GNOS software KMS display driver. (GPLv2)
 *
 * The driver owns the virtual CRTC/plane/encoder/connector pipeline and the
 * copy loop that puts a committed framebuffer on the scanout target.  A
 * hardware driver (see drm_svga.h) hands it a different target through
 * drm_scanout_set() during probe.
 */

#ifndef INCLUDE_DRM_SIMPLEDRM_SIMPLEDRM_H_
#define INCLUDE_DRM_SIMPLEDRM_SIMPLEDRM_H_

#include <stdint.h>

/* Bring up the fallback software device (card0 + renderD128) and its KMS
 * pipeline.  Skipped by the caller when a hardware driver already registered. */
int simpledrm_init(void);

struct drm_scanout {
    volatile uint32_t *base;
    uint32_t           w, h, pitch;
};

/* Hand the copy path a target.  Pass base == NULL to go back to fbcon. */
int drm_scanout_set(volatile uint32_t *base, uint32_t w, uint32_t h, uint32_t pitch);

/* Resolve the current target; falls back to fbcon's geometry when unset. */
void drm_scanout_get(volatile uint32_t **base, uint32_t *w, uint32_t *h, uint32_t *pitch);

/* Called from the timer tick/refresh thread to put the live buffer on screen. */
void drm_dummy_refresh(void);
void drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh, uint32_t dstep);

#endif /* INCLUDE_DRM_SIMPLEDRM_SIMPLEDRM_H_ */
