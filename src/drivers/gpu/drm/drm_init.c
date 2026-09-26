/*
 * drm_init.c - bringing up the display device this kernel actually has.
 * (GPLv2)
 *
 * GNOS has no GPU driver of its own, so the DRM device it presents is a
 * software one: a real KMS pipeline -- CRTC, primary plane, encoder,
 * connector with a mode list -- whose scanout target is the framebuffer the
 * bootloader set up.  A client that speaks DRM (Xorg, wlroots, anything
 * built on libdrm) therefore finds an ordinary KMS device and needs no
 * special case; the pixels it commits are copied to the console framebuffer
 * by the refresh path at the bottom of this file.
 *
 * Two details are the whole trick:
 *
 *  - A client's renderer draws into the buffer it allocated and never commits
 *    again, so the picture has to be re-copied on a timer.  That is the
 *    refresh thread (and drm_vblank_tick, which also retires page flips and
 *    delivers their completion events).
 *  - Nothing here may start a kernel thread during boot: KMS setup runs before
 *    the scheduler exists.  The refresh thread is created lazily by the first
 *    page flip, which by definition happens once user space -- and a working
 *    scheduler -- is there.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm.h"
#include "drm_device.h"
#include "drm_devtmpfs.h" /* device/class/devtmpfs shim -> GNOS VFS */
#include "drm_fourcc.h"
#include "drm_init.h"
#include "drm_mode.h"
#include "drm_print.h"
#include "drm_vsnprintf.h"
#include "heap.h"
#include "kstring.h"
#include "vfs.h"
#include "sysfs.h"
#include "fbcon.h"

/* proc.h: WAIT_SLEEP is 5 there; match the signature exactly rather than
 * pulling the header in here. */
#define DRM_WAIT_SLEEP 5
void sched_block_timeout(uint32_t why, uint64_t ticks);

/* The DRM core is always built into this kernel. */
#ifndef CONFIG_DRM
#    define CONFIG_DRM 1
#endif

extern int                      drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs);
extern void                     drm_vblank_tick(void);
extern struct drm_display_mode *drm_mode_create(struct drm_device *dev);
extern void                     drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode);
extern int                      drm_read(struct drm_file *file_priv, char *buf, size_t count, size_t *offset);

/* ------------------------------------------------------- device registry */

/* Every registered device, in registration order.  Most of the kernel asks
 * for "the" device, which means the first one. */
#define DRM_MAX_DEVICES 16

static struct drm_device *drm_device_list[DRM_MAX_DEVICES];
static spinlock_t         drm_device_list_lock = {0};

void drm_device_list_add(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i] == NULL) {
            drm_device_list[i] = dev;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

void drm_device_list_remove(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i] == dev) {
            drm_device_list[i] = NULL;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

struct drm_device *drm_get_singleton(void)
{
    spin_lock(&drm_device_list_lock);

    dbg_puts("DRMSING: list[0]=");
    dbg_puts_hex((uint64_t)(uintptr_t)drm_device_list[0]);
    dbg_puts("\r\n");

    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];

        if (dev != NULL) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
    }

    spin_unlock(&drm_device_list_lock);
    return NULL;
}

/* The device behind a given /dev/dri node, which is how a driver finds the
 * device an ioctl arrived on. */
struct drm_device *drm_get_device_by_minor(int type, int index)
{
    spin_lock(&drm_device_list_lock);

    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];

        if (dev == NULL) { continue; }

        if (type == DRM_MINOR_PRIMARY && dev->primary != NULL && dev->primary->index == index) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
        if (type == DRM_MINOR_RENDER && dev->render != NULL && dev->render->index == index) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
    }

    spin_unlock(&drm_device_list_lock);
    return NULL;
}

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
 * giving the pages back. */
static void drm_dummy_gem_free_object(struct drm_gem_object *obj)
{
    if (obj == NULL) { return; }

    aligned_free(obj->backing);
    obj->backing = NULL;

    free(obj->dma_buf);
    obj->dma_buf = NULL;
}

/*
 * PRIME import: the only buffers this device can import are the ones it
 * exported, so the "dma-buf" handed back is really the GEM object itself.
 */
static struct drm_gem_object *drm_dummy_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    (void)dev;

    if (obj == NULL) { return NULL; }

    drm_gem_object_get(obj);
    return obj;
}

/* The commands this device answers; anything else gets ENOTTY. */
static const struct drm_ioctl_desc drm_dummy_ioctls[] = {
    {DRM_IOCTL_VERSION,                drm_version,                      0                    },
    {DRM_IOCTL_GET_MAGIC,              drm_getmagic,                     DRM_AUTH             },
    {DRM_IOCTL_SET_VERSION,            drm_setversion,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_SET_MASTER,             drm_setmaster,                    0                    },
    {DRM_IOCTL_DROP_MASTER,            drm_dropmaster,                   0                    },
    {DRM_IOCTL_AUTH_MAGIC,             drm_authmagic,                    DRM_AUTH             },
    {DRM_IOCTL_GEM_CLOSE,              drm_gem_close_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_FLINK,              drm_gem_flink_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_OPEN,               drm_gem_open_ioctl,               DRM_AUTH             },
    {DRM_IOCTL_GET_CAP,                drm_get_cap,                      0                    },
    {DRM_IOCTL_SET_CLIENT_CAP,         drm_set_client_cap,               0                    },
    {DRM_IOCTL_WAIT_VBLANK,            drm_wait_vblank_ioctl,            0                    },
    {DRM_IOCTL_MODE_GETRESOURCES,      drm_mode_getresources,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCRTC,           drm_mode_getcrtc,                 DRM_AUTH             },
    {DRM_IOCTL_MODE_SETCRTC,           drm_mode_setcrtc,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR,            drm_mode_cursor_ioctl,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETENCODER,        drm_mode_getencoder,              DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCONNECTOR,      drm_mode_getconnector,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPROPERTY,       drm_mode_getproperty_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPROPBLOB,       drm_mode_getpropblob_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_CREATEPROPBLOB,    drm_mode_createpropblob_ioctl,    DRM_AUTH             },
    {DRM_IOCTL_MODE_DESTROYPROPBLOB,   drm_mode_destroypropblob_ioctl,   DRM_AUTH             },
    {DRM_IOCTL_MODE_GETGAMMA,          drm_mode_getgamma_ioctl,          DRM_AUTH             },
    {DRM_IOCTL_MODE_SETGAMMA,          drm_mode_setgamma_ioctl,          DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB,             drm_mode_getfb,                   DRM_AUTH             },
    {DRM_IOCTL_MODE_ADDFB,             drm_mode_addfb,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_RMFB,              drm_mode_rmfb,                    DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_PAGE_FLIP,         drm_mode_page_flip_ioctl,         DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_DIRTYFB,           drm_mode_dirtyfb,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPLANE,          drm_mode_getplane,                DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPLANE,          drm_mode_setplane,                DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB2,            drm_mode_addfb2,                  DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_mode_obj_getproperties_ioctl, DRM_AUTH             },
    {DRM_IOCTL_MODE_OBJ_SETPROPERTY,   drm_mode_obj_setproperty_ioctl,   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR2,           drm_mode_cursor2_ioctl,           DRM_AUTH             },
    {DRM_IOCTL_MODE_ATOMIC,            drm_mode_atomic_ioctl,            DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB2,            drm_mode_getfb2_ioctl,            DRM_AUTH             },
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

static void drm_refresh_thread(void *arg);
void        drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh, uint32_t dstep);

static unsigned long g_refresh_count;
static volatile int g_cursor_on;   /* TEMPORARY: mirrors sw_cursor.on */

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

/* Copy the live buffer to the console framebuffer and stamp the cursor on
 * top.  Called from the timer tick as well as from the refresh thread. */
void drm_dummy_refresh(void)
{
    uint32_t       dw = 0, dh = 0, dpitch = 0;
    uint32_t      *dst;
    const uint8_t *src;
    uint32_t       w, h, dstep;

    /* Heartbeat: once a client hands us a framebuffer this counts up every
     * frame.  A compositor that is up but showing nothing shows up here as
     * a count that never moves. */
    if ((++g_refresh_count & 63) == 0) {
        dbg_puts("REFRESH n=");
        dbg_puts_hex(g_refresh_count);
        dbg_puts(" fb=");
        dbg_puts_hex((uint64_t)(uintptr_t)g_scan_fb);
        dbg_puts(" cur=");
        dbg_puts_dec(g_cursor_on);
        /* TEMPORARY Xorg debugging: checksum of the scanout source, so a
         * black screen can be traced to an unpainted dumb buffer vs a
         * broken copy path. */
        if (g_scan_fb && g_scan_fb->obj[0] && g_scan_fb->obj[0]->backing) {
            const uint8_t *bp = (const uint8_t *)g_scan_fb->obj[0]->backing;
            uint32_t sum = 0, n = g_scan_fb->pitches[0] * g_scan_fb->height;
            if (n > 262144) n = 262144;
            for (uint32_t i = 0; i < n; i++) sum += bp[i];
            dbg_puts(" srcsum=");
            dbg_puts_hex(sum);
        }
        dbg_puts("\n");
    }

    if (g_scan_fb == NULL || g_scan_fb->obj[0] == NULL || g_scan_fb->obj[0]->backing == NULL) { return; }

    fbcon_geometry(&dw, &dh, &dpitch);
    dst = (uint32_t *)fbcon_fb();
    src = (const uint8_t *)g_scan_fb->obj[0]->backing;
    if (dst == NULL || src == NULL || dw == 0 || dpitch == 0) { return; }

    /* Clip: the client may have chosen a mode larger than the display was
     * booted with. */
    w     = (g_scan_fb->width > dw) ? dw : g_scan_fb->width;
    h     = (g_scan_fb->height > dh) ? dh : g_scan_fb->height;
    dstep = dpitch / 4;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t       *d = dst + (uint64_t)row * dstep;
        const uint32_t *s = (const uint32_t *)(src + (uint64_t)row * g_scan_fb->pitches[0]);

        for (uint32_t col = 0; col < w; col++) { d[col] = s[col]; }
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
    static int refresh_thread_up;
    uint32_t       dw = 0, dh = 0, dpitch = 0;
    uint32_t      *dst;
    const uint8_t *src;
    uint32_t       w, h, dstep;

    if (!refresh_thread_up) {
        refresh_thread_up = 1;
        if (kthread_create("drm-refresh", drm_refresh_thread, NULL) == NULL) { refresh_thread_up = 0; }
    }

    (void)crtc;
    (void)event;
    (void)flags;

    if (fb == NULL) { return 0; }
    if (fb->obj[0] == NULL || fb->obj[0]->backing == NULL) { return -EINVAL; }

    g_scan_fb = fb;

    /* While a client framebuffer owns the scanout, fbcon must not paint
     * into the same memory the refresh thread keeps overwriting -- a stray
     * login-prompt line or a "^C" echo used to flicker through for one
     * refresh cycle.  Restore fbcon as soon as the console framebuffer is
     * scanned out again. */
    fbcon_suppress(fb->obj[0]->backing != (const uint8_t *)fbcon_fb());

    fbcon_geometry(&dw, &dh, &dpitch);
    dst = (uint32_t *)fbcon_fb();
    src = (const uint8_t *)fb->obj[0]->backing;
    if (dst == NULL || src == NULL || dw == 0 || dpitch == 0) { return -EINVAL; }

    w     = (fb->width > dw) ? dw : fb->width;
    h     = (fb->height > dh) ? dh : fb->height;
    dstep = dpitch / 4;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t       *d = dst + (uint64_t)row * dstep;
        const uint32_t *s = (const uint32_t *)(src + (uint64_t)row * fb->pitches[0]);

        for (uint32_t col = 0; col < w; col++) { d[col] = s[col]; }
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

    if (crtc == NULL || crtc->primary == NULL) { return; }
    if (crtc->primary->state != NULL) { fb = crtc->primary->state->fb; }
    if (fb == NULL) { return; }

    if (drm_dummy_page_flip(crtc, fb, NULL, 0) == 0) {
        /* Keep the cursor above whatever was just scanned out. */
        uint32_t dw = 0, dh = 0, dpitch = 0;

        fbcon_geometry(&dw, &dh, &dpitch);
        uint32_t *dst = (uint32_t *)fbcon_fb();
        if (dst != NULL && dw != 0 && dpitch != 0) { drm_dummy_draw_cursor(dst, dw, dh, dpitch / 4); }
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

static int drm_dummy_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *bo, uint32_t width, uint32_t height,
                                int32_t hot_x, int32_t hot_y)
{
    (void)crtc;

    spin_lock(&sw_cursor_lock);
    sw_cursor.bo    = bo;
    sw_cursor.w     = width;
    sw_cursor.h     = height;
    sw_cursor.hot_x = hot_x;
    sw_cursor.hot_y = hot_y;
    sw_cursor.on    = (bo != NULL);
    g_cursor_on     = sw_cursor.on;   /* TEMPORARY: heartbeat visibility */
    spin_unlock(&sw_cursor_lock);

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

    return 0;
}

/* Draw the cursor over @dst.  Pixels whose high byte is mostly clear are
 * transparent; everything else is copied as it is. */
void drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh, uint32_t dstep)
{
    const uint32_t *src;
    uint32_t        cw, ch;
    int32_t         cx, cy;

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

    if (cx < 0) { cx = 0; }
    if (cy < 0) { cy = 0; }
    if ((uint32_t)cx >= dw || (uint32_t)cy >= dh) { return; }

    if (cw > dw - (uint32_t)cx) { cw = dw - (uint32_t)cx; }
    if (ch > dh - (uint32_t)cy) { ch = dh - (uint32_t)cy; }

    for (uint32_t row = 0; row < ch; row++) {
        uint32_t       *d    = dst + (uint64_t)(cy + row) * dstep + cx;
        const uint32_t *srow = src + (uint64_t)row * sw_cursor.w;

        for (uint32_t col = 0; col < cw; col++) {
            uint32_t px = srow[col];

            if ((px >> 24) > 127) { d[col] = px; }
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
        const struct dummy_mode_cfg *cfg = &dummy_modes[i];
        struct drm_display_mode     *mode = drm_mode_create(dev);

        if (mode == NULL) { return -ENOMEM; }

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
                if ((uint32_t)dummy_modes[i].hdisplay == fb_w && (uint32_t)dummy_modes[i].vdisplay == fb_h) {
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
                    mode->hdisplay = fb_w;
                    mode->vdisplay = fb_h;
                    mode->htotal   = fb_w;
                    mode->vtotal   = fb_h;
                    mode->clock    = (int)((uint64_t)fb_w * fb_h * 60 / 1000);
                    mode->vrefresh = 60;
                    mode->flags    = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
                    mode->type    = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
                    mode->status  = MODE_OK;

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

    dbg_puts("RTHREAD start\n");

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
    int                   ret;

    memset(&pipeline_crtc, 0, sizeof(pipeline_crtc));
    memset(&pipeline_primary_plane, 0, sizeof(pipeline_primary_plane));
    memset(&pipeline_encoder, 0, sizeof(pipeline_encoder));
    memset(&pipeline_connector, 0, sizeof(pipeline_connector));

    /* The primary plane, bound to CRTC 0. */
    ret = drm_plane_init(dev, &pipeline_primary_plane, 1, /* possible_crtcs = bit 0 */
                         NULL, primary_formats, sizeof(primary_formats) / sizeof(primary_formats[0]), NULL,
                         DRM_PLANE_TYPE_PRIMARY, "primary");
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

    ret = drm_crtc_init_with_planes(dev, &pipeline_crtc, &pipeline_primary_plane, NULL, &pipeline_crtc_helper, "CRTC-0");
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

    DRM_INFO("KMS pipeline: CRTC-%u + primary plane-%u + encoder-%u + connector-%u (%u modes)\n", pipeline_crtc.base.id,
             pipeline_primary_plane.base.id, pipeline_encoder.base.id, pipeline_connector.base.id,
             sizeof(dummy_modes) / sizeof(dummy_modes[0]));

    return 0;
}

/* ------------------------------------------------------------ VFS plumbing */

/* read() on a DRM node dequeues events: it blocks while there are none and
 * returns 0 once the file is being closed, which is what libdrm expects. */
int64_t drm_dev_read(void *file, void *addr, size_t offset, size_t size)
{
    return (int64_t)drm_read((struct drm_file *)file, (char *)addr, size, &offset);
}

size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;

    return 0;
}

int drm_dev_ioctl(void *file, size_t req, void *arg)
{
    struct drm_file   *file_priv = (struct drm_file *)file;
    struct drm_device *dev;

    if (file_priv == NULL) { return -ENODEV; }

    dev = drm_get_singleton();
    if (dev == NULL) { return -ENODEV; }

    return drm_ioctl(dev, (unsigned int)req, arg, file_priv);
}

/*
 * A VFS node is shared by every process that has it open, so per-open state
 * cannot live on the node: it is allocated here and handed back in
 * @private_data instead.
 */
int drm_dev_open(void *node_ptr, uint64_t flags, void **private_data)
{
    struct drm_device *dev;
    struct drm_file   *file;
    int                ret;

    (void)flags;

    if (private_data == NULL) { return -EINVAL; }
    *private_data = NULL;

    dev = drm_get_singleton();
    if (dev == NULL) { return -ENODEV; }

    file = malloc(sizeof(*file));
    if (file == NULL) { return -ENOMEM; }
    memset(file, 0, sizeof(*file));

    ret = drm_open(dev, file);
    if (ret != 0) {
        free(file);
        return ret;
    }

    /* A compositor running as root on the primary node is trusted for
     * DRM_AUTH commands straight away: Weston goes straight to GETRESOURCES
     * after opening, without issuing SET_MASTER first, and would otherwise
     * decide the device has nothing to show.  Render nodes stay
     * unauthenticated, which is the point of them. */
    {
        struct vfs_node *node = (struct vfs_node *)node_ptr;
        process_t       *proc = process_current();

        if (node != NULL && node->name[0] != '\0' && !strncmp(node->name, "card", 4) && proc != NULL
            && proc->uid == 0) {
            file->authenticated = true;
        }
    }

    *private_data = file;
    return 0;
}

void drm_dev_release(void *node_ptr, void *private_data)
{
    (void)node_ptr;

    if (private_data != NULL) {
        drm_release((struct drm_file *)private_data);
        free(private_data);
    }
}

int drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    (void)ctx;
    (void)flags;

    return drm_dev_ioctl(private_data, req, arg);
}

int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;

    return drm_dev_read(private_data, addr, offset, size);
}

int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;

    return (int64_t)drm_dev_write(private_data, addr, offset, size);
}

int drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;

    return drm_dev_poll(private_data, events);
}

int drm_dev_poll(void *file, size_t events)
{
    /* Real readiness: readable only while a drm event (vblank/page flip)
     * is queued for this open file description.  The stub that always
     * returned 0 combined with the bridge's old revents logic made the
     * card0 fd report readable forever, and Xorg spun reading it. */
    extern unsigned int drm_poll(struct drm_file *file_priv, unsigned int events);
    return (int)drm_poll((struct drm_file *)file, (unsigned int)events);
}

/* mmap: the offset handed out by MAP_DUMB identifies the buffer, and the
 * backing memory is identity-mapped, so its address is the answer. */
void *drm_dev_mmap(void *file, size_t offset, size_t size, int flags)
{
    struct drm_file       *file_priv = (struct drm_file *)file;
    struct drm_gem_object *obj;
    void                  *result;

    (void)size;
    (void)flags;

    if (file_priv == NULL) { return NULL; }

    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);
    if (obj == NULL) { return NULL; }

    result = obj->backing;
    drm_gem_object_put(obj);

    return result;
}

void *drm_dev_file_mmap(void *ctx, void *private_data, uint64_t offset, uint64_t size, int flags, struct vm_area *vma)
{
    struct drm_device     *dev       = (struct drm_device *)ctx;
    struct drm_file       *file_priv = (struct drm_file *)private_data;
    struct drm_gem_object *obj;

    (void)size;
    (void)flags;

    if (dev == NULL) { dev = drm_get_singleton(); }

    dbg_puts("DRMMAP: dev=");
    dbg_puts_hex((uint64_t)(uintptr_t)dev);
    dbg_puts(" fp=");
    dbg_puts_hex((uint64_t)(uintptr_t)file_priv);
    dbg_puts(" off=");
    dbg_puts_hex(offset);
    dbg_puts(" sz=");
    dbg_puts_hex((uint64_t)size);
    dbg_puts(" fl=");
    dbg_puts_hex((uint64_t)flags);
    dbg_puts(" vma=");
    dbg_puts_hex((uint64_t)(uintptr_t)vma);
    dbg_puts("\r\n");

    if (dev == NULL || file_priv == NULL || vma == NULL) { return NULL; }

    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);

    dbg_puts("DRMMAP: obj=");
    dbg_puts_hex((uint64_t)(uintptr_t)obj);
    dbg_puts(" backing=");
    dbg_puts_hex((obj != NULL) ? (uint64_t)(uintptr_t)obj->backing : 0);
    dbg_puts("\r\n");

    if (obj == NULL || obj->backing == NULL) { return NULL; }

    /* The VMA keeps the object alive: unmap drops the reference. */
    vma->vm_private_data = obj;

    return obj->backing;
}

/* GNOS's VFS has no per-open directory callbacks -- the DRM open path runs
 * through drm_dev_open above -- so these two are inert. */
void drm_vfs_open_cb(void *parent, const char *name, void *node_ptr)
{
    (void)parent;
    (void)name;
    (void)node_ptr;
}

void drm_vfs_close_cb(void *current)
{
    (void)current;
}

/* -------------------------------------------------------------- DRM class */

/* udev finds the device nodes through /sys/class/drm and expects this
 * uevent variable to tell it what kind of node it is looking at. */
static int drm_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    (void)dev;

    return add_uevent_var(env, "DEVTYPE=drm_minor");
}

struct class drm_class = {
    .name       = "drm",
    .dev_uevent = drm_device_uevent,
};
int drm_class_registered = 0;

/* ------------------------------------------------------------------- init */

#if CONFIG_DRM

int drm_init(void)
{
    /* Only the class: the software device must not claim card0 or renderD128
     * before a hardware driver has had its chance to probe. */
    if (!drm_class_registered) {
        int ret = class_register(&drm_class);

        if (ret != EOK) { return ret; }
        drm_class_registered = 1;
    }

    return 0;
}

int drm_init_fallback(void)
{
    struct drm_device *dev = drm_dev_alloc(&drm_dummy_driver);
    int                ret;

    if (dev == NULL) {
        DRM_ERROR("Failed to allocate DRM device\n");
        return -ENOMEM;
    }

    ret = drm_dev_register(dev, 0);
    if (ret != 0) {
        DRM_ERROR("Failed to register DRM device: %d\n", ret);
        free(dev);
        return ret;
    }

    ret = drm_dummy_kms_setup(dev);
    if (ret != 0) {
        DRM_ERROR("Failed to set up KMS pipeline: %d\n", ret);
        free(dev);
        return ret;
    }

    /* Publish the card under /sys/class/drm/card0 the way real KMS
     * devices do: status/enabled answer udev's and libdrm's first
     * questions about a connector. */
    sysfs_add_file("class/drm/card0/status", sysfs_gen_card0_status, NULL);
    sysfs_add_file("class/drm/card0/enabled", sysfs_gen_card0_status, NULL);

    drm_device_list_add(dev);

    dbg_puts("DRMINIT: fallback device=");
    dbg_puts_hex((uint64_t)(uintptr_t)dev);
    dbg_puts(" list[0]=");
    dbg_puts_hex((uint64_t)(uintptr_t)drm_device_list[0]);
    dbg_puts("\r\n");

    return 0;
}

#else

int drm_init(void)
{
    return -ENODEV;
}

int drm_init_fallback(void)
{
    return -ENODEV;
}

#endif /* CONFIG_DRM */
