/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_svga.h — VMware SVGA II scanout driver interface. (GPLv2)
 *
 * A hardware driver for the one display device QEMU can expose besides the
 * VBE one: VMware SVGA II (15ad:0405).  Where drm_vbe.c only changes the
 * geometry of the framebuffer the bootloader already set up, this one owns
 * its own VRAM and hands the DRM core a real scanout target to copy into —
 * see drm_scanout_set() in drm_init.h.
 *
 * The driver is deliberately small: probe, mode set, and the FIFO UPDATE
 * notification.  No 2D acceleration, no hardware cursor.
 */

#ifndef INCLUDE_DRM_DRM_SVGA_H_
#define INCLUDE_DRM_DRM_SVGA_H_

#include <stdint.h>

/*
 * Probe the device and, if it answers, program a mode and take over the
 * scanout target.  Returns 0 when there is no SVGA II device (or it refused
 * the mode we asked for) and non-zero when the scanout now lives in SVGA
 * VRAM.
 *
 * Must run after pci_init() and after fbcon has its geometry, and before the
 * KMS pipeline is built: the pipeline sizes its copy loop from whatever
 * drm_scanout_get() reports.
 */
int svga_init(void);

/* Did svga_init() succeed? */
int svga_present(void);

/*
 * Tell the device that a rectangle changed.  SVGA only guarantees the host
 * repaints when it is told to; without this a guest that writes VRAM
 * directly may keep showing whatever was there when it was enabled.
 */
void svga_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/*
 * Hardware cursor.  drm_init.c feeds these from its cursor_set/cursor_move
 * callbacks and stops painting the software cursor while one of these is
 * showing (svga_cursor_active()).
 */
int  svga_cursor_define(uint32_t id, uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y,
                        const uint32_t *argb);
void svga_cursor_move(int32_t x, int32_t y);
void svga_cursor_show(int on);
int  svga_cursor_active(void);

/*
 * Copy a guest buffer to the screen without touching the CPU.
 *
 * @src must live in the direct map (every GEM backing buffer does).  On
 * success @fence_out receives a fence that has been issued but not waited
 * on; svga_fence_wait() blocks until it lands.  Returns non-zero when there
 * is no accelerated path, in which case the caller must do the copy itself.
 */
int svga_blit(const void *src, uint32_t pitch, uint32_t w, uint32_t h, uint32_t *fence_out);
int svga_fence_wait(uint32_t fence, uint32_t max_ticks);

/* Is the accelerated copy available?  Only set by svga_selftest(). */
int svga_accelerated(void);

/*
 * Prove the accelerated copy works before anything depends on it, and use
 * the proof to settle how GMR page descriptors are encoded.  Runs during
 * boot next to the other selftests; does nothing when there is no device.
 */
void svga_selftest(void);

#endif /* INCLUDE_DRM_DRM_SVGA_H_ */
