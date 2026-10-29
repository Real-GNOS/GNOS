/* SPDX-License-Identifier: GPL-2.0 */
/*
 * simpledrm.c - the GNOS software display driver. (GPLv2)
 *
 * This is the device the kernel presents when no hardware display driver
 * claimed the scanout: a real KMS pipeline -- CRTC, primary plane, encoder,
 * connector with a mode list -- whose target is either the SVGA VRAM (when
 * drm_svga.c took it over) or the framebuffer the bootloader set up.
 *
 * A client paints into its own buffer and does not commit again, so the
 * refresh thread copies that buffer to the scanout once per tick.  Nothing
 * may start a kernel thread during boot: KMS setup runs before the scheduler
 * exists, and the refresh thread is created lazily by the first page flip.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm.h"
#include "drm_device.h"
#include "drm_fourcc.h"
#include "drm_init.h"
#include "drm_mode.h"
#include "drm_prime.h"
#include "drm_print.h"
#include "drm_svga.h"
#include "drm_vbe.h"
#include "drm_vsnprintf.h"
#include "fbcon.h"
#include "heap.h"
#include "kstring.h"
#include "simpledrm.h"
#include "sysfs.h"
#include "vfs.h"

/* proc.h: WAIT_SLEEP is 5 there; match the signature exactly rather than
 * pulling the header in here. */
#define DRM_WAIT_SLEEP 5
void sched_block_timeout(uint32_t why, uint64_t ticks);

/* The DRM core is always built into this kernel. */
#ifndef CONFIG_DRM
#define CONFIG_DRM 1
#endif

#if CONFIG_DRM

extern int                      drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs);
extern void                     drm_vblank_tick(void);
extern struct drm_display_mode *drm_mode_create(struct drm_device *dev);
extern void drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode);

/* --------------------------------------------------------- the device itself */

static int drm_dummy_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

static void drm_dummy_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

static void drm_dummy_lastclose(struct drm_device *dev)
{
    (void)dev;
}

/* Buffers here are ordinary page-aligned allocations, so freeing one is just
 * giving the pages back.  The dma-buf, if the object was exported, owns the
 * memory on this side of the reference pair and is freed by dma_buf_put()
 * when its last descriptor closes -- never here. */
static void drm_dummy_gem_free_object(struct drm_gem_object *obj)
{
    if (obj == NULL) {
        return;
    }

    aligned_free(obj->backing);
    obj->backing = NULL;
}

/*
 * PRIME import: the objects this device can import without a generic wrapper
 * are the ones it exported itself.  Anything else is handled by
 * drm_gem_prime_import_dev(), which builds a borrow wrapper.
 */
static struct drm_gem_object *drm_dummy_gem_prime_import(struct drm_device *dev,
                                                         struct dma_buf    *dma_buf)
{
    return drm_gem_prime_import_same_dev(dev, dma_buf);
}

/* The commands this device answers; anything else gets ENOTTY. */
static const struct drm_ioctl_desc drm_dummy_ioctls[] = {
    {DRM_IOCTL_VERSION, drm_version, 0},
    {DRM_IOCTL_GET_MAGIC, drm_getmagic, DRM_AUTH},
    {DRM_IOCTL_SET_VERSION, drm_setversion, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_SET_MASTER, drm_setmaster, 0},
    {DRM_IOCTL_DROP_MASTER, drm_dropmaster, 0},
    {DRM_IOCTL_AUTH_MAGIC, drm_authmagic, DRM_AUTH},
    {DRM_IOCTL_GEM_CLOSE, drm_gem_close_ioctl, DRM_AUTH},
    {DRM_IOCTL_GEM_FLINK, drm_gem_flink_ioctl, DRM_AUTH},
    {DRM_IOCTL_GEM_OPEN, drm_gem_open_ioctl, DRM_AUTH},
    {DRM_IOCTL_GET_CAP, drm_get_cap, 0},
    {DRM_IOCTL_SET_CLIENT_CAP, drm_set_client_cap, 0},
    {DRM_IOCTL_WAIT_VBLANK, drm_wait_vblank_ioctl, 0},
    {DRM_IOCTL_MODE_GETRESOURCES, drm_mode_getresources, DRM_AUTH},
    {DRM_IOCTL_MODE_GETCRTC, drm_mode_getcrtc, DRM_AUTH},
    {DRM_IOCTL_MODE_SETCRTC, drm_mode_setcrtc, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR, drm_mode_cursor_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_GETENCODER, drm_mode_getencoder, DRM_AUTH},
    {DRM_IOCTL_MODE_GETCONNECTOR, drm_mode_getconnector, DRM_AUTH},
    {DRM_IOCTL_MODE_GETPROPERTY, drm_mode_getproperty_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_GETPROPBLOB, drm_mode_getpropblob_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_CREATEPROPBLOB, drm_mode_createpropblob_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_DESTROYPROPBLOB, drm_mode_destroypropblob_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_GETGAMMA, drm_mode_getgamma_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_SETGAMMA, drm_mode_setgamma_ioctl, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB, drm_mode_getfb, DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB, drm_mode_addfb, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_RMFB, drm_mode_rmfb, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_PAGE_FLIP, drm_mode_page_flip_ioctl, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_DIRTYFB, drm_mode_dirtyfb, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res, DRM_AUTH},
    {DRM_IOCTL_MODE_GETPLANE, drm_mode_getplane, DRM_AUTH},
    {DRM_IOCTL_MODE_SETPLANE, drm_mode_setplane, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB2, drm_mode_addfb2, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_mode_obj_getproperties_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_OBJ_SETPROPERTY, drm_mode_obj_setproperty_ioctl, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR2, drm_mode_cursor2_ioctl, DRM_AUTH},
    {DRM_IOCTL_MODE_ATOMIC, drm_mode_atomic_ioctl, DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB2, drm_mode_getfb2_ioctl, DRM_AUTH},
};

static struct drm_driver drm_dummy_driver = {
    .name             = "drm",
    .desc             = "GNOS software DRM",
    .date             = "20260722",
    .major            = 1,
    .minor            = 0,
    .patchlevel       = 0,
    .driver_features  = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_PRIME | DRIVER_RENDER,
    .open             = drm_dummy_open,
    .postclose        = drm_dummy_postclose,
    .lastclose        = drm_dummy_lastclose,
    .gem_free_object  = drm_dummy_gem_free_object,
    .gem_prime_import = drm_dummy_gem_prime_import,
    .dumb_create      = drm_gem_dumb_create,
    .dumb_map_offset  = drm_gem_dumb_map_offset,
    .dumb_destroy     = drm_gem_dumb_destroy,
    .ioctls           = drm_dummy_ioctls,
    .num_ioctls       = sizeof(drm_dummy_ioctls) / sizeof(drm_dummy_ioctls[0]),
};

/* ---------------------------------------------------------- the pipeline */

static struct drm_crtc      pipeline_crtc;
static struct drm_plane     pipeline_primary_plane;
static struct drm_encoder   pipeline_encoder;
static struct drm_connector pipeline_connector;

/* Modes are data, not code: adding one is a table entry. */
struct dummy_mode_cfg {
    const char *name;
    int         clock;
    int         hdisplay;
    int         hsync_start;
    int         hsync_end;
    int         htotal;
    int         vdisplay;
    int         vsync_start;
    int         vsync_end;
    int         vtotal;
    int         vrefresh;
    unsigned    flags;
    unsigned    type;
};

/* The scanout bridge: the pixel copy at the end goes through the console
 * framebuffer -- the same one text mode draws into, so a successful commit
 * visibly replaces the tty. */
#include "fbcon.h"

/* Whatever is being scanned out right now. */
static struct drm_framebuffer *g_scan_fb;

/* Where it is scanned out to: the console framebuffer unless a hardware
 * driver claimed the target first.  drm_svga.c sets this during probe,
 * which happens before the pipeline below is built. */
static struct drm_scanout g_scanout;

int drm_scanout_set(volatile uint32_t *base, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (base != NULL && (w == 0 || h == 0 || pitch == 0)) {
        return -EINVAL;
    }

    g_scanout.base  = base;
    g_scanout.w     = w;
    g_scanout.h     = h;
    g_scanout.pitch = pitch;
    return 0;
}

/* The one place that decides which memory a committed framebuffer is copied
 * into.  Unset means the console framebuffer, which is also what every path
 * did before a hardware driver existed. */
void drm_scanout_get(volatile uint32_t **base, uint32_t *w, uint32_t *h, uint32_t *pitch)
{
    if (g_scanout.base == NULL) {
        *base = (volatile uint32_t *)fbcon_fb();
        fbcon_geometry(w, h, pitch);
        return;
    }

    *base  = g_scanout.base;
    *w     = g_scanout.w;
    *h     = g_scanout.h;
    *pitch = g_scanout.pitch;
}

static void drm_refresh_thread(void *arg);
void        drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh, uint32_t dstep);

/* /sys/class/drm/card0/{status,enabled}: the card reads enabled while a
 * client framebuffer is scanned out, disabled when the console is back.
 * File-scope because it reads g_scan_fb (declared below). */
static void sysfs_gen_card0_status(char *buf, uint32_t cap, uint32_t *len)
{
    const char *s = (g_scan_fb != NULL) ? "enabled\n" : "disabled\n";
    while (*s && (uint32_t)(*len) + 1 < cap)
        buf[(*len)++] = *s++;
    if ((uint32_t)(*len) < cap)
        buf[*len] = '\0';
}

/* Copy the live buffer to the scanout target and stamp the cursor on top.
 * Called from the timer tick as well as from the refresh thread. */
void drm_dummy_refresh(void)
{
    uint32_t           dw = 0, dh = 0, dpitch = 0;
    uint32_t          *dst;
    const uint8_t     *src;
    uint32_t           w, h, dstep;
    volatile uint32_t *vdst;

    if (g_scan_fb == NULL || g_scan_fb->obj[0] == NULL || g_scan_fb->obj[0]->backing == NULL) {
        return;
    }

    drm_scanout_get(&vdst, &dw, &dh, &dpitch);
    dst = (uint32_t *)vdst;
    src = (const uint8_t *)g_scan_fb->obj[0]->backing;
    if (dst == NULL || src == NULL || dw == 0 || dpitch == 0) {
        return;
    }

    /* Clip: the client may have chosen a mode larger than the display was
     * booted with. */
    w     = (g_scan_fb->width > dw) ? dw : g_scan_fb->width;
    h     = (g_scan_fb->height > dh) ? dh : g_scan_fb->height;
    dstep = dpitch / 4;

    /* Let the device do the copy when it can.  svga_blit() only claims to
     * work after svga_selftest() has compared a blit byte for byte, so the
     * CPU loop below stays as the answer for everything else: no device, a
     * source outside the direct map, a FIFO too small for the page table. */
    if (svga_blit(src, g_scan_fb->pitches[0], w, h, NULL) != 0) {
        for (uint32_t row = 0; row < h; row++) {
            uint32_t       *d = dst + (uint64_t)row * dstep;
            const uint32_t *s = (const uint32_t *)(src + (uint64_t)row * g_scan_fb->pitches[0]);

            for (uint32_t col = 0; col < w; col++) {
                d[col] = s[col];
            }
        }

        svga_update(0, 0, w, h);
    }

    drm_dummy_draw_cursor(dst, dw, dh, dstep);
}

/*
 * A page flip here means "start showing this framebuffer".  It also starts
 * the refresh thread the first time it happens, which is the earliest point
 * at which a thread can survive.
 */
static int drm_dummy_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                               struct drm_pending_vblank_event *event, uint32_t flags)
{
    static int         refresh_thread_up;
    uint32_t           dw = 0, dh = 0, dpitch = 0;
    uint32_t          *dst;
    const uint8_t     *src;
    uint32_t           w, h, dstep;
    volatile uint32_t *vdst;

    if (!refresh_thread_up) {
        refresh_thread_up = 1;
        if (kthread_create("drm-refresh", drm_refresh_thread, NULL) == NULL) {
            refresh_thread_up = 0;
        }
    }

    (void)crtc;
    (void)event;
    (void)flags;

    if (fb == NULL) {
        return 0;
    }
    if (fb->obj[0] == NULL || fb->obj[0]->backing == NULL) {
        return -EINVAL;
    }

    g_scan_fb = fb;

    /* While a client framebuffer owns the scanout, fbcon must not paint
     * into the same memory the refresh thread keeps overwriting -- a stray
     * login-prompt line or a "^C" echo used to flicker through for one
     * refresh cycle.  Restore fbcon as soon as the console framebuffer is
     * scanned out again. */
    fbcon_suppress(fb->obj[0]->backing != (const uint8_t *)fbcon_fb());

    drm_scanout_get(&vdst, &dw, &dh, &dpitch);
    dst = (uint32_t *)vdst;
    src = (const uint8_t *)fb->obj[0]->backing;
    if (dst == NULL || src == NULL || dw == 0 || dpitch == 0) {
        return -EINVAL;
    }

    w     = (fb->width > dw) ? dw : fb->width;
    h     = (fb->height > dh) ? dh : fb->height;
    dstep = dpitch / 4;

    if (svga_blit(src, fb->pitches[0], w, h, NULL) != 0) {
        for (uint32_t row = 0; row < h; row++) {
            uint32_t       *d = dst + (uint64_t)row * dstep;
            const uint32_t *s = (const uint32_t *)(src + (uint64_t)row * fb->pitches[0]);

            for (uint32_t col = 0; col < w; col++) {
                d[col] = s[col];
            }
        }

        svga_update(0, 0, w, h);
    }

    return 0;
}

/*
 * Called once per software vblank: copy whatever the CRTC's primary plane is
 * showing.  A renderer that paints into its own buffer and never commits
 * again depends entirely on this, otherwise the display freezes on the first
 * frame it ever saw.
 */
static void drm_dummy_vblank_blit(struct drm_crtc *crtc)
{
    struct drm_framebuffer *fb = NULL;

    if (crtc == NULL || crtc->primary == NULL) {
        return;
    }
    if (crtc->primary->state != NULL) {
        fb = crtc->primary->state->fb;
    }
    if (fb == NULL) {
        return;
    }

    if (drm_dummy_page_flip(crtc, fb, NULL, 0) == 0) {
        /* Keep the cursor above whatever was just scanned out. */
        uint32_t           dw = 0, dh = 0, dpitch = 0;
        volatile uint32_t *vdst;

        drm_scanout_get(&vdst, &dw, &dh, &dpitch);
        uint32_t *dst = (uint32_t *)vdst;
        if (dst != NULL && dw != 0 && dpitch != 0) {
            drm_dummy_draw_cursor(dst, dw, dh, dpitch / 4);
        }
    }
}

/* --------------------------------------------------------- software cursor */

/* There is no cursor overlay in hardware, so the cursor plane is drawn by
 * hand after each copy: cursor_set keeps the ARGB source, cursor_move the
 * position, and the blit stamps it. */
static spinlock_t sw_cursor_lock;

static struct {
    struct drm_gem_object *bo;
    uint32_t               w, h;
    int32_t                hot_x, hot_y;
    int32_t                x, y; /* top-left of the image, hotspot applied */
    bool                   on;
} sw_cursor;

static int drm_dummy_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *bo, uint32_t width,
                                uint32_t height, int32_t hot_x, int32_t hot_y)
{
    (void)crtc;

    spin_lock(&sw_cursor_lock);
    sw_cursor.bo    = bo;
    sw_cursor.w     = width;
    sw_cursor.h     = height;
    sw_cursor.hot_x = hot_x;
    sw_cursor.hot_y = hot_y;
    sw_cursor.on    = (bo != NULL);
    spin_unlock(&sw_cursor_lock);

    /* The device can draw this itself.  The software copy below stays
     * armed as the answer for a machine with no SVGA II; with one, it
     * stands down rather than painting a second cursor over the hardware's
     * (svga_cursor_active()). */
    if (svga_present()) {
        if (bo != NULL && bo->backing != NULL) {
            if (svga_cursor_define(1, width, height, hot_x, hot_y, (const uint32_t *)bo->backing) ==
                0) {
                svga_cursor_show(1);
                svga_cursor_move(sw_cursor.x, sw_cursor.y);
            } else {
                svga_cursor_show(0); /* FIFO refused it: fall back to software */
            }
        } else {
            svga_cursor_show(0);
        }
    }

    return 0;
}

static int drm_dummy_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y)
{
    (void)crtc;

    spin_lock(&sw_cursor_lock);
    /* The position a client gives is where the hotspot goes; what we draw
     * from is the image's top-left corner. */
    sw_cursor.x = x - sw_cursor.hot_x;
    sw_cursor.y = y - sw_cursor.hot_y;
    spin_unlock(&sw_cursor_lock);

    if (svga_present()) {
        svga_cursor_move(x, y);
    }

    return 0;
}

/* Draw the cursor over @dst.  Pixels whose high byte is mostly clear are
 * transparent; everything else is copied as it is.  Does nothing while the
 * SVGA II device is drawing the cursor itself. */
void drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh, uint32_t dstep)
{
    const uint32_t *src;
    uint32_t        cw, ch;
    int32_t         cx, cy;

    if (svga_cursor_active()) {
        return;
    }

    spin_lock(&sw_cursor_lock);
    if (!sw_cursor.on || sw_cursor.bo == NULL || sw_cursor.bo->backing == NULL) {
        spin_unlock(&sw_cursor_lock);
        return;
    }
    src = (const uint32_t *)sw_cursor.bo->backing;
    cw  = sw_cursor.w;
    ch  = sw_cursor.h;
    cx  = sw_cursor.x;
    cy  = sw_cursor.y;
    spin_unlock(&sw_cursor_lock);

    if (cx < 0) {
        cx = 0;
    }
    if (cy < 0) {
        cy = 0;
    }
    if ((uint32_t)cx >= dw || (uint32_t)cy >= dh) {
        return;
    }

    if (cw > dw - (uint32_t)cx) {
        cw = dw - (uint32_t)cx;
    }
    if (ch > dh - (uint32_t)cy) {
        ch = dh - (uint32_t)cy;
    }

    for (uint32_t row = 0; row < ch; row++) {
        uint32_t       *d    = dst + (uint64_t)(cy + row) * dstep + cx;
        const uint32_t *srow = src + (uint64_t)row * sw_cursor.w;

        for (uint32_t col = 0; col < cw; col++) {
            uint32_t px = srow[col];

            if ((px >> 24) > 127) {
                d[col] = px;
            }
        }
    }
}

static const struct drm_crtc_helper_funcs pipeline_crtc_helper = {
    .page_flip   = drm_dummy_page_flip,
    .vblank      = drm_dummy_vblank_blit,
    .cursor_set  = drm_dummy_cursor_set,
    .cursor_move = drm_dummy_cursor_move,
};

static const struct dummy_mode_cfg dummy_modes[] = {
    {
        .name        = "1920x1080",
        .clock       = 148500,
        .hdisplay    = 1920,
        .hsync_start = 2008,
        .hsync_end   = 2052,
        .htotal      = 2200,
        .vdisplay    = 1080,
        .vsync_start = 1084,
        .vsync_end   = 1089,
        .vtotal      = 1125,
        .vrefresh    = 60,
        .flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
        .type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER,
    },
    {
        .name        = "1280x720",
        .clock       = 74250,
        .hdisplay    = 1280,
        .hsync_start = 1390,
        .hsync_end   = 1430,
        .htotal      = 1650,
        .vdisplay    = 720,
        .vsync_start = 725,
        .vsync_end   = 730,
        .vtotal      = 750,
        .vrefresh    = 60,
        .flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
        .type        = DRM_MODE_TYPE_DRIVER,
    },
};

static int drm_dummy_kms_add_modes(struct drm_device *dev, struct drm_connector *connector)
{
    (void)dev;

    for (unsigned int i = 0; i < sizeof(dummy_modes) / sizeof(dummy_modes[0]); i++) {
        const struct dummy_mode_cfg *cfg  = &dummy_modes[i];
        struct drm_display_mode     *mode = drm_mode_create(dev);

        if (mode == NULL) {
            return -ENOMEM;
        }

        strncpy(mode->name, cfg->name, DRM_DISPLAY_MODE_LEN - 1);
        mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
        mode->clock                          = cfg->clock;
        mode->hdisplay                       = cfg->hdisplay;
        mode->hsync_start                    = cfg->hsync_start;
        mode->hsync_end                      = cfg->hsync_end;
        mode->htotal                         = cfg->htotal;
        mode->vdisplay                       = cfg->vdisplay;
        mode->vsync_start                    = cfg->vsync_start;
        mode->vsync_end                      = cfg->vsync_end;
        mode->vtotal                         = cfg->vtotal;
        mode->vrefresh                       = cfg->vrefresh;
        mode->flags                          = cfg->flags;
        mode->type                           = cfg->type;
        mode->status                         = MODE_OK;

        drm_mode_probed_add(connector, mode);
    }

    /* Offer the resolution the bootloader actually programmed, so a client
     * always finds a mode matching the real display however the table above
     * is set up. */
    {
        uint32_t fb_w = 0, fb_h = 0, fb_pitch = 0;

        fbcon_geometry(&fb_w, &fb_h, &fb_pitch);

        if (fb_w > 0 && fb_h > 0) {
            int duplicate = 0;

            for (unsigned int i = 0; i < sizeof(dummy_modes) / sizeof(dummy_modes[0]); i++) {
                if ((uint32_t)dummy_modes[i].hdisplay == fb_w &&
                    (uint32_t)dummy_modes[i].vdisplay == fb_h) {
                    duplicate = 1;
                    break;
                }
            }

            if (!duplicate) {
                struct drm_display_mode *mode = drm_mode_create(dev);

                if (mode != NULL) {
                    char namebuf[DRM_DISPLAY_MODE_LEN];

                    snprintf(namebuf, sizeof(namebuf), "%ux%u", fb_w, fb_h);
                    strncpy(mode->name, namebuf, DRM_DISPLAY_MODE_LEN - 1);
                    mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
                    mode->hdisplay                       = fb_w;
                    mode->vdisplay                       = fb_h;
                    mode->htotal                         = fb_w;
                    mode->vtotal                         = fb_h;
                    mode->clock                          = (int)((uint64_t)fb_w * fb_h * 60 / 1000);
                    mode->vrefresh                       = 60;
                    mode->flags  = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
                    mode->type   = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
                    mode->status = MODE_OK;

                    drm_mode_probed_add(connector, mode);
                }
            }
        }
    }

    return 0;
}

/* One frame per tick: retire flips, deliver their events, then refresh. */
static void drm_refresh_thread(void *arg)
{
    (void)arg;

    DRM_INFO("refresh thread started\n");

    for (;;) {
        sched_block_timeout((uint32_t)DRM_WAIT_SLEEP, 1);
        drm_vblank_tick();
        drm_dummy_refresh();
    }
}

static int drm_dummy_kms_setup(struct drm_device *dev)
{
    static const uint32_t primary_formats[] = {
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ARGB8888,
        DRM_FORMAT_RGB888,
        DRM_FORMAT_RGB565,
    };
    int ret;

    memset(&pipeline_crtc, 0, sizeof(pipeline_crtc));
    memset(&pipeline_primary_plane, 0, sizeof(pipeline_primary_plane));
    memset(&pipeline_encoder, 0, sizeof(pipeline_encoder));
    memset(&pipeline_connector, 0, sizeof(pipeline_connector));

    /* Reach the hardware through PCI before building the pipeline: find
     * the display device, switch its spaces on and probe VBE.  Without an
     * answer mode changes must stay refused rather than half-apply.
     *
     * SVGA wins when it is there.  It was probed earlier (see kernel.c) and
     * already claimed the scanout target -- its own VRAM, not the console
     * framebuffer -- so asking VBE to reshape the console framebuffer would
     * move a surface nothing is scanning out any more. */
    if (svga_present()) {
        DRM_INFO("svga ii owns the scanout: skipping VBE probe\n");
    } else {
        int vid = vbe_pci_probe();
        if (vid) {
            DRM_INFO("vbe interface version 0x%x: mode changes enabled\n", vid);
        } else {
            DRM_INFO("no VBE device: modes stay as the bootloader set them\n");
        }
    }

    /* The primary plane, bound to CRTC 0. */
    ret =
        drm_plane_init(dev, &pipeline_primary_plane, 1, /* possible_crtcs = bit 0 */
                       NULL, primary_formats, sizeof(primary_formats) / sizeof(primary_formats[0]),
                       NULL, DRM_PLANE_TYPE_PRIMARY, "primary");
    if (ret != 0) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        return ret;
    }

    pipeline_primary_plane.state = malloc(sizeof(*pipeline_primary_plane.state));
    if (pipeline_primary_plane.state == NULL) {
        DRM_ERROR("Failed to alloc primary plane state\n");
        return -ENOMEM;
    }
    memset(pipeline_primary_plane.state, 0, sizeof(*pipeline_primary_plane.state));
    pipeline_primary_plane.state->plane            = &pipeline_primary_plane;
    pipeline_primary_plane.state->crtc             = &pipeline_crtc;
    pipeline_primary_plane.state->rotation         = 0;
    pipeline_primary_plane.state->alpha            = 0xFFFF;
    pipeline_primary_plane.state->pixel_blend_mode = 0;
    pipeline_primary_plane.state->visible          = true;

    ret = drm_crtc_init_with_planes(dev, &pipeline_crtc, &pipeline_primary_plane, NULL,
                                    (void *)&pipeline_crtc_helper, "CRTC-0");
    if (ret != 0) {
        DRM_ERROR("Failed to init CRTC: %d\n", ret);
        return ret;
    }

    pipeline_crtc.state = malloc(sizeof(*pipeline_crtc.state));
    if (pipeline_crtc.state == NULL) {
        DRM_ERROR("Failed to alloc CRTC state\n");
        return -ENOMEM;
    }
    memset(pipeline_crtc.state, 0, sizeof(*pipeline_crtc.state));
    pipeline_crtc.state->crtc   = &pipeline_crtc;
    pipeline_crtc.state->active = false;
    pipeline_crtc.state->enable = false;

    /* A virtual encoder and connector: there is no cable. */
    ret = drm_encoder_init(dev, &pipeline_encoder, NULL, DRM_MODE_ENCODER_VIRTUAL, "encoder-0");
    if (ret != 0) {
        DRM_ERROR("Failed to init encoder: %d\n", ret);
        return ret;
    }
    pipeline_encoder.possible_crtcs = 1;
    pipeline_encoder.crtc           = &pipeline_crtc;

    ret = drm_connector_init(dev, &pipeline_connector, NULL, DRM_MODE_CONNECTOR_VIRTUAL);
    if (ret != 0) {
        DRM_ERROR("Failed to init connector: %d\n", ret);
        return ret;
    }
    pipeline_connector.status                 = connector_status_connected;
    pipeline_connector.display_info_width_mm  = 500;
    pipeline_connector.display_info_height_mm = 280;

    pipeline_connector.state = malloc(sizeof(*pipeline_connector.state));
    if (pipeline_connector.state == NULL) {
        DRM_ERROR("Failed to alloc connector state\n");
        return -ENOMEM;
    }
    memset(pipeline_connector.state, 0, sizeof(*pipeline_connector.state));
    pipeline_connector.state->connector    = &pipeline_connector;
    pipeline_connector.state->crtc         = &pipeline_crtc;
    pipeline_connector.state->best_encoder = &pipeline_encoder;

    ret = drm_connector_attach_encoder(&pipeline_connector, &pipeline_encoder);
    if (ret != 0) {
        DRM_ERROR("Failed to attach encoder: %d\n", ret);
        return ret;
    }

    ret = drm_dummy_kms_add_modes(dev, &pipeline_connector);
    if (ret != 0) {
        DRM_ERROR("Failed to add modes: %d\n", ret);
        return ret;
    }

    ret = drm_vblank_init(dev, 1);
    if (ret != 0) {
        DRM_ERROR("Failed to init vblank: %d\n", ret);
        return ret;
    }

    drm_connector_register(&pipeline_connector);

    DRM_INFO("KMS pipeline: CRTC-%u + primary plane-%u + encoder-%u + connector-%u (%u modes)\n",
             pipeline_crtc.base.id, pipeline_primary_plane.base.id, pipeline_encoder.base.id,
             pipeline_connector.base.id, sizeof(dummy_modes) / sizeof(dummy_modes[0]));

    return 0;
}

/* Bring up the fallback software device and the KMS pipeline on top of it. */
int simpledrm_init(void)
{
    struct drm_device *dev = drm_dev_alloc(&drm_dummy_driver);
    int                ret;

    if (dev == NULL) {
        DRM_ERROR("Failed to allocate DRM device\n");
        return -ENOMEM;
    }

    /* Build the KMS pipeline first: a registered /dev/dri device must
     * already expose its modes and be fully usable. */
    ret = drm_dummy_kms_setup(dev);
    if (ret != 0) {
        DRM_ERROR("Failed to set up KMS pipeline: %d\n", ret);
        drm_dev_unregister(dev);
        return ret;
    }

    ret = drm_dev_register(dev, 0);
    if (ret != 0) {
        DRM_ERROR("Failed to register DRM device: %d\n", ret);
        drm_dev_unregister(dev);
        return ret;
    }

    /* Publish the card under /sys/class/drm/card0 the way real KMS devices
     * do: status/enabled answer udev's and libdrm's first questions. */
    sysfs_add_file("class/drm/card0/status", sysfs_gen_card0_status, NULL);
    sysfs_add_file("class/drm/card0/enabled", sysfs_gen_card0_status, NULL);

    drm_device_list_add(dev);

    return 0;
}

#else

int simpledrm_init(void)
{
    return -ENODEV;
}

#endif /* CONFIG_DRM */
