/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nep1.c — NeoRunst compatibility loader for GNOS. (GPLv2)
 *
 * Loads a NEP1 flat-PIE image, calls nep_main(KernelApi*) with the table in
 * RDI, and implements all 23 API functions right here in user space:
 * drawing goes straight into a DRM dumb framebuffer, events come from the
 * evdev devices, files through POSIX.  No kernel GUI code anywhere -- the
 * loader IS the window system, which fits NeoRunst's single-program model.
 *
 * NEP1 header (little endian):
 *   +0x00 u32 "NEP1"  +0x04 u16 version=1  +0x06 u16 hdr_size=32
 *   +0x08 u32 entry_offset (file offset)   +0x0C u32 image_size (<16MiB)
 *   +0x20 .. the flat PIE image; the tail up to image_size is BSS.
 *
 * The 5x7 font is runstker's own glyph table (5 bytes per column, 95
 * printable ASCII glyphs), extracted from DAT_14002af28.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* ================================ DRM ==================================== */

typedef struct { unsigned long capability, value; } drm_get_cap_t;

typedef struct {
    unsigned long fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    unsigned count_fbs, count_crtcs, count_connectors, count_encoders;
    unsigned min_width, max_width, min_height, max_height;
} drm_mode_card_res_t;

typedef struct {
    unsigned clock;
    unsigned short hdisplay, hsync_start, hsync_end, htotal, hskew;
    unsigned short vdisplay, vsync_start, vsync_end, vtotal, vscan;
    unsigned vrefresh, flags, type;
    char name[32];
} drm_mode_modeinfo_t;

typedef struct {
    unsigned long encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    unsigned count_modes, count_props, count_encoders;
    unsigned encoder_id, connector_id, connector_type, connector_type_id;
    unsigned connection, mm_width, mm_height, subpixel, pad;
} drm_mode_get_connector_t;

typedef struct {
    unsigned long set_connectors_ptr;
    unsigned count_connectors;
    unsigned crtc_id, fb_id, x, y, gamma_size, mode_valid;
    drm_mode_modeinfo_t mode;
} drm_mode_crtc_t;

typedef struct {
    unsigned height, width, bpp, flags;
    unsigned handle, pitch;
    unsigned long size;
} drm_mode_create_dumb_t;

typedef struct { unsigned handle, pad; unsigned long offset; } drm_mode_map_dumb_t;
typedef struct { unsigned fb_id, width, height, pixel_format, flags;
                 unsigned handles[4], pitches[4], offsets[4];
                 unsigned long modifier[4]; } drm_mode_fb_cmd2_t;

#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0
#define DRM_IOCTL_MODE_GETCONNECTOR 0xc05064a7
#define DRM_IOCTL_MODE_SETCRTC      0xc06864a2
#define DRM_IOCTL_MODE_CREATE_DUMB  0xc02064b2
#define DRM_IOCTL_MODE_MAP_DUMB     0xc01064b3
#define DRM_IOCTL_MODE_ADDFB2       0xc06864b8
#define DRM_FORMAT_XRGB8888         0x34325258u

static int       drm_fd = -1;
static unsigned *fb;
static unsigned   fb_w, fb_h, fb_pitch;
static unsigned long fb_size;

/* ============================ NEP1 header ================================ */

#define NEP1_MAGIC   0x3150454Eu
#define NEP1_VERSION 1
#define NEP1_HDR     32
#define NEP1_MAXIMG  0xFFFFFFu

/* ============================ the ABI ==================================== */

struct ApiEvent {
    uint32_t event_type, ctrl_id, x, y, key;
};
#define EV_BTN_CLICK 1
#define EV_KEY       2
#define EV_MOUSE     4

struct KernelApi {
    const uint8_t *(*get_version)(void);
    uint32_t (*create_window)(int32_t, int32_t, uint32_t, uint32_t, const char *);
    void (*draw_pixel)(uint32_t, uint32_t, uint32_t, uint32_t);
    void (*fill_rect)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*draw_text)(uint32_t, uint32_t, uint32_t, const char *, uint32_t);
    void (*draw_button)(uint32_t, int32_t, int32_t, uint32_t, uint32_t, const char *);
    void (*draw_input)(uint32_t, int32_t, int32_t, uint32_t, uint32_t);
    uint32_t (*poll_event)(struct ApiEvent *);
    void (*exit_app)(uint32_t);
    uint32_t (*create_button)(uint32_t, int32_t, int32_t, uint32_t, uint32_t,
                              const char *, uint64_t);
    uint32_t (*create_input)(uint32_t, int32_t, int32_t, uint32_t, uint32_t);
    uint32_t (*create_label)(uint32_t, int32_t, int32_t, const char *, uint32_t);
    uint32_t (*get_input_text)(uint32_t, uint32_t, char *, uint32_t);
    void (*set_ctrl_text)(uint32_t, uint32_t, const char *);
    uint32_t (*get_event)(uint32_t, struct ApiEvent *);
    void (*delete_ctrl)(uint32_t, uint32_t);
    void (*set_ctrl_visible)(uint32_t, uint32_t, uint32_t);
    uint32_t (*write_file)(const uint8_t *, const uint8_t *, uint32_t);
    uint32_t (*read_file)(const uint8_t *, uint8_t *, uint32_t);
    uint32_t (*list_directory)(const uint8_t *, uint8_t *, uint32_t);
    uint32_t (*random_u32)(void);
    uint64_t (*random_u64)(void);
    void (*random_fill)(uint8_t *, uint32_t);
};

/* ============================ the font =================================== */

/* runstker's own glyph table: 95 glyphs (0x20..0x7E), 5 columns of 7 rows,
 * bit0 = top row, 6px advance.  Extracted from DAT_14002af28. */
static const uint8_t nep_font[95 * 5] = {
#include "nep_font.h"
};

/* ============================ state ====================================== */

#define MAXWIN  8
#define MAXCTRL 64
#define MAXEV   64

typedef struct {
    int      used, visible;
    int32_t  x, y;
    uint32_t w, h;
    char     title[64];
} win_t;

enum { C_BUTTON = 1, C_INPUT = 2, C_LABEL = 3 };

typedef struct {
    int      used, visible, focused, type;
    uint32_t win, id;
    int32_t  x, y;
    uint32_t w, h;
    char     text[128];
    uint32_t color;
    uint64_t cb;
} ctrl_t;

static win_t  g_win[MAXWIN];
static ctrl_t g_ctrl[MAXCTRL];
static uint32_t g_next_win = 1, g_next_ctrl = 1;
static int    g_hidden;
static unsigned g_mouse_x = 8, g_mouse_y = 8;
static struct ApiEvent g_evq[MAXEV];
static unsigned g_ev_head, g_ev_count;
static int    g_running = 1;

static int32_t  cur_wx, cur_wy;      /* current window's client origin */

static void draw_cursor(void);
static void ev_push(const struct ApiEvent *e);
static void pump_input(void);
static void pump_keyboard(void);

/* ============================ drawing ==================================== */

static inline void fb_px(unsigned x, unsigned y, uint32_t c)
{
    if (x < fb_w && y < fb_h)
        fb[y * (fb_pitch / 4) + x] = c;
}

static void fb_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t c)
{
    for (unsigned ry = 0; ry < h; ry++)
        for (unsigned rx = 0; rx < w; rx++)
            fb_px(x + rx, y + ry, c);
}

static void draw_char(int x, int y, char ch, uint32_t c)
{
    if (ch < 0x20 || ch > 0x7E)
        ch = '?';
    const uint8_t *g = &nep_font[(ch - 0x20) * 5];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row))
                fb_px((unsigned)(x + col), (unsigned)(y + row), c);
    }
}

static void draw_text_raw(int x, int y, const char *s, uint32_t c)
{
    for (; *s; s++, x += 6)
        draw_char(x, y, *s, c);
}

/* Win95 bevel box. */
static void bevel(int x, int y, unsigned w, unsigned h, int raised)
{
    uint32_t light = 0x00FFFFFF, dark = 0x00808080, black = 0x00000000;
    uint32_t tl = raised ? light : dark, br = raised ? dark : light;
    for (unsigned i = 0; i < w; i++) {
        fb_px((unsigned)x + i, (unsigned)y, tl);
        fb_px((unsigned)x + i, (unsigned)(y + (int)h - 1), br);
    }
    for (unsigned i = 0; i < h; i++) {
        fb_px((unsigned)x, (unsigned)(y + i), tl);
        fb_px((unsigned)(x + (int)w - 1), (unsigned)(y + i), br);
    }
    fb_px((unsigned)x, (unsigned)y, black);
    fb_px((unsigned)(x + (int)w - 1), (unsigned)y, black);
    fb_px((unsigned)x, (unsigned)(y + (int)h - 1), black);
    fb_px((unsigned)(x + (int)w - 1), (unsigned)(y + (int)h - 1), black);
}

static void draw_window_frame(win_t *w)
{
    if (!w->visible)
        return;
    fb_fill((unsigned)w->x, (unsigned)w->y, w->w, w->h, 0x00C0C0C0);
    bevel(w->x, w->y, w->w, w->h, 1);
    fb_fill((unsigned)w->x + 3, (unsigned)w->y + 3, w->w - 6, 18, 0x00000080);
    draw_text_raw(w->x + 6, w->y + 8, w->title, 0x00FFFFFF);
}

static void paint_ctrl(ctrl_t *ct)
{
    if (!ct->visible)
        return;
    win_t *w = &g_win[ct->win - 1];
    int x = w->x + ct->x, y = w->y + ct->y + 24;   /* below the title bar */

    if (ct->type == C_BUTTON) {
        fb_fill((unsigned)x, (unsigned)y, ct->w, ct->h, 0x00C0C0C0);
        bevel(x, y, ct->w, ct->h, 1);
        unsigned tw = (unsigned)strlen(ct->text) * 6;
        draw_text_raw(x + (int)((ct->w - tw) / 2), y + (int)(ct->h / 2) - 3,
                      ct->text, 0x00000000);
    } else if (ct->type == C_INPUT) {
        fb_fill((unsigned)x, (unsigned)y, ct->w, ct->h, 0x00FFFFFF);
        bevel(x, y, ct->w, ct->h, 0);
        draw_text_raw(x + 4, y + (int)(ct->h / 2) - 3, ct->text, 0x00000000);
        if (ct->focused) {
            int cx = x + 4 + (int)strlen(ct->text) * 6;
            fb_fill((unsigned)cx, (unsigned)y + 3, 2, ct->h - 6, 0x00000000);
        }
    } else {                                          /* label */
        draw_text_raw(x, y, ct->text, ct->color);
    }
}

static void repaint_all(void)
{
    if (g_hidden) {
        fb_fill(0, 0, fb_w, fb_h, 0x00000000);
        return;
    }
    fb_fill(0, 0, fb_w, fb_h, 0x00000000);
    for (int i = 0; i < MAXWIN; i++)
        if (g_win[i].used && g_win[i].visible)
            draw_window_frame(&g_win[i]);
    for (int i = 0; i < MAXCTRL; i++)
        if (g_ctrl[i].used && g_ctrl[i].visible)
            paint_ctrl(&g_ctrl[i]);
    draw_cursor();
}

/* ============================ cursor ===================================== */

static void draw_cursor(void)
{
    for (int r = 0; r < 10; r++) {
        unsigned w = (unsigned)(10 - r) / 2 + 1;
        fb_fill(g_mouse_x, g_mouse_y + (unsigned)r, w, 1, 0x00FFFFFF);
        fb_px(g_mouse_x + w, g_mouse_y + (unsigned)r, 0x00000000);
    }
}

/* ============================ input ====================================== */

struct input_event { long tv_sec, tv_usec; unsigned short type, code; int value; };
#define IEV_SYN 0x00
#define IEV_KEY 0x01
#define IEV_REL 0x02
#define SYN_REPORT 0
#define REL_X 0x00
#define REL_Y 0x01
#define BTN_LEFT 0x110
#define KEY_ESC 1

static int ev_fd = -1;

static void pump_input(void)
{
    struct input_event ie;
    while (read(ev_fd, &ie, sizeof ie) == (ssize_t)sizeof ie) {
        if (ie.type == IEV_REL) {
            if (ie.code == REL_X) {
                long nx = (long)g_mouse_x + ie.value;
                g_mouse_x = (unsigned)(nx < 0 ? 0 : (nx >= (long)fb_w ? fb_w - 1 : nx));
            } else if (ie.code == REL_Y) {
                long ny = (long)g_mouse_y + ie.value;
                g_mouse_y = (unsigned)(ny < 0 ? 0 : (ny >= (long)fb_h ? fb_h - 1 : ny));
            }
            repaint_all();
        } else if (ie.type == IEV_KEY && ie.value == 1 && ie.code == BTN_LEFT) {
            for (int i = MAXCTRL - 1; i >= 0; i--) {
                ctrl_t *ct = &g_ctrl[i];
                if (!ct->used || !ct->visible)
                    continue;
                win_t *w = &g_win[ct->win - 1];
                int x = w->x + ct->x, y = w->y + ct->y + 24;
                if (g_mouse_x >= (unsigned)x &&
                    g_mouse_x < (unsigned)(x + (int)ct->w) &&
                    g_mouse_y >= (unsigned)y &&
                    g_mouse_y < (unsigned)(y + (int)ct->h)) {
                    struct ApiEvent e = { EV_BTN_CLICK, ct->id,
                                          g_mouse_x, g_mouse_y, 0 };
                    ev_push(&e);
                    if (ct->cb) {
                        void (*cb)(uint32_t, uint32_t) =
                            (void (*)(uint32_t, uint32_t))(uintptr_t)ct->cb;
                        cb(ct->win, ct->id);
                    }
                    break;
                }
            }
        }
    }
}

/* keycodes -> ASCII (the subset text fields need) */
static int key_to_ascii(unsigned code, int shift)
{
    if (code >= 2 && code <= 10)  return '0' + (int)code - 1;
    if (code == 11)               return '0';
    if (code >= 16 && code <= 25) return (shift ? 'Q' : 'q') + (int)(code - 16);
    if (code >= 30 && code <= 38) return (shift ? 'A' : 'a') + (int)(code - 30);
    if (code >= 44 && code <= 50) return (shift ? 'Z' : 'z') + (int)(code - 44);
    if (code == 39) return shift ? ':' : ';';
    if (code == 40) return shift ? '"' : '\'';
    if (code == 57) return ' ';
    return -1;
}

static void pump_keyboard(void)
{
    struct input_event ie;
    while (read(ev_fd, &ie, sizeof ie) == (ssize_t)sizeof ie) {
        if (ie.type != IEV_KEY)
            continue;
        if (ie.value == 1 && ie.code == KEY_ESC) {
            g_running = 0;
            return;
        }
        struct ApiEvent e = { EV_KEY, 0, 0, 0, ie.code };
        ev_push(&e);
        if (ie.value == 1) {
            for (int i = 0; i < MAXCTRL; i++) {
                ctrl_t *ct = &g_ctrl[i];
                if (!(ct->used && ct->visible && ct->type == C_INPUT && ct->focused))
                    continue;
                size_t len = strlen(ct->text);
                if (ie.code == 14 && len) {             /* backspace */
                    ct->text[len - 1] = 0;
                } else {
                    int ch = key_to_ascii(ie.code, 0);
                    if (ch >= 0x20 && len + 1 < sizeof(ct->text)) {
                        ct->text[len] = (char)ch;
                        ct->text[len + 1] = 0;
                    }
                }
                break;
            }
        }
    }
}

/* ============================ events ===================================== */

static void ev_push(const struct ApiEvent *e)
{
    if (g_ev_count < MAXEV)
        g_evq[(g_ev_head + g_ev_count) % MAXEV] = *e, g_ev_count++;
}

static int ev_pop(struct ApiEvent *out)
{
    if (!g_ev_count)
        return 0;
    *out = g_evq[g_ev_head];
    g_ev_head = (g_ev_head + 1) % MAXEV;
    g_ev_count--;
    return 1;
}

/* ============================ API ======================================== */

static const uint8_t *api_get_version(void)
{
    return (const uint8_t *)"NeoRunst v0.0.7";
}

static uint32_t api_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                  const char *title)
{
    int slot = -1;
    for (int i = 0; i < MAXWIN; i++)
        if (!g_win[i].used) { slot = i; break; }
    if (slot < 0)
        return 0;
    win_t *wn = &g_win[slot];
    memset(wn, 0, sizeof *wn);
    wn->used = wn->visible = 1;
    wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    snprintf(wn->title, sizeof wn->title, "%s", title ? title : "");
    uint32_t id = g_next_win++;
    cur_wx = x; cur_wy = y;
    draw_window_frame(wn);
    printf("nep: window %u \"%s\"\n", id, wn->title);
    fflush(stdout);
    return id;
}

static void api_draw_pixel(uint32_t win, uint32_t x, uint32_t y, uint32_t c)
{
    (void)win;
    fb_px(cur_wx + x, cur_wy + 24 + y, c);
}

static void api_fill_rect(uint32_t win, uint32_t x, uint32_t y, uint32_t w,
                          uint32_t h, uint32_t c)
{
    (void)win;
    fb_fill(cur_wx + x, cur_wy + 24 + y, w, h, c);
}

static void api_draw_text(uint32_t win, uint32_t x, uint32_t y, const char *t,
                          uint32_t c)
{
    (void)win;
    draw_text_raw(cur_wx + (int)x, cur_wy + 24 + (int)y, t ? t : "", c);
}

static void api_draw_button(uint32_t win, int32_t x, int32_t y, uint32_t w,
                            uint32_t h, const char *label)
{
    (void)win;
    fb_fill(cur_wx + (unsigned)x, cur_wy + 24 + (unsigned)y, w, h, 0x00C0C0C0);
    bevel(cur_wx + x, cur_wy + 24 + y, w, h, 1);
    unsigned tw = (unsigned)strlen(label ? label : "") * 6;
    draw_text_raw(cur_wx + x + (int)((w - tw) / 2), cur_wy + 24 + y + (int)(h / 2) - 3,
                  label ? label : "", 0x00000000);
}

static void api_draw_input(uint32_t win, int32_t x, int32_t y, uint32_t w,
                           uint32_t h)
{
    (void)win;
    fb_fill(cur_wx + (unsigned)x, cur_wy + 24 + (unsigned)y, w, h, 0x00FFFFFF);
    bevel(cur_wx + x, cur_wy + 24 + y, w, h, 0);
}

static uint32_t api_poll_event(struct ApiEvent *ev)
{
    return ev_pop(ev);
}

static void api_exit_app(uint32_t code)
{
    (void)code;
    g_hidden = 1;
    repaint_all();
}

static uint32_t api_create_button(uint32_t win, int32_t x, int32_t y,
                                  uint32_t w, uint32_t h, const char *label,
                                  uint64_t cb)
{
    int slot = -1;
    for (int i = 0; i < MAXCTRL; i++)
        if (!g_ctrl[i].used) { slot = i; break; }
    if (slot < 0)
        return 0;
    ctrl_t *ct = &g_ctrl[slot];
    memset(ct, 0, sizeof *ct);
    ct->used = ct->visible = 1;
    ct->type = C_BUTTON;
    ct->win = win; ct->x = x; ct->y = y; ct->w = w; ct->h = h;
    snprintf(ct->text, sizeof ct->text, "%s", label ? label : "");
    ct->cb = cb;
    ct->id = g_next_ctrl++;
    paint_ctrl(ct);
    return ct->id;
}

static uint32_t api_create_input(uint32_t win, int32_t x, int32_t y,
                                 uint32_t w, uint32_t h)
{
    int slot = -1;
    for (int i = 0; i < MAXCTRL; i++)
        if (!g_ctrl[i].used) { slot = i; break; }
    if (slot < 0)
        return 0;
    ctrl_t *ct = &g_ctrl[slot];
    memset(ct, 0, sizeof *ct);
    ct->used = ct->visible = ct->focused = 1;
    ct->type = C_INPUT;
    ct->win = win; ct->x = x; ct->y = y; ct->w = w; ct->h = h;
    ct->id = g_next_ctrl++;
    paint_ctrl(ct);
    return ct->id;
}

static uint32_t api_create_label(uint32_t win, int32_t x, int32_t y,
                                 const char *text, uint32_t color)
{
    int slot = -1;
    for (int i = 0; i < MAXCTRL; i++)
        if (!g_ctrl[i].used) { slot = i; break; }
    if (slot < 0)
        return 0;
    ctrl_t *ct = &g_ctrl[slot];
    memset(ct, 0, sizeof *ct);
    ct->used = ct->visible = 1;
    ct->type = C_LABEL;
    ct->win = win; ct->x = x; ct->y = y;
    snprintf(ct->text, sizeof ct->text, "%s", text ? text : "");
    ct->color = color;
    ct->id = g_next_ctrl++;
    paint_ctrl(ct);
    return ct->id;
}

static ctrl_t *find_ctrl(uint32_t win, uint32_t id)
{
    for (int i = 0; i < MAXCTRL; i++)
        if (g_ctrl[i].used && g_ctrl[i].id == id &&
            (win == 0 || g_ctrl[i].win == win))
            return &g_ctrl[i];
    return NULL;
}

static uint32_t api_get_input_text(uint32_t win, uint32_t id, char *buf,
                                   uint32_t max)
{
    ctrl_t *ct = find_ctrl(win, id);
    if (!ct || !buf || !max)
        return 0;
    snprintf(buf, max, "%s", ct->text);
    return (uint32_t)strlen(buf);
}

static void api_set_ctrl_text(uint32_t win, uint32_t id, const char *text)
{
    ctrl_t *ct = find_ctrl(win, id);
    if (!ct)
        return;
    snprintf(ct->text, sizeof ct->text, "%s", text ? text : "");
    paint_ctrl(ct);
}

static uint32_t api_get_event(uint32_t win, struct ApiEvent *ev)
{
    (void)win;
    return ev_pop(ev);
}

static void api_delete_ctrl(uint32_t win, uint32_t id)
{
    ctrl_t *ct = find_ctrl(win, id);
    if (ct)
        memset(ct, 0, sizeof *ct);
    repaint_all();
}

static void api_set_ctrl_visible(uint32_t win, uint32_t id, uint32_t visible)
{
    ctrl_t *ct = find_ctrl(win, id);
    if (ct)
        ct->visible = visible ? 1 : 0;
    repaint_all();
}

static uint32_t api_write_file(const uint8_t *path, const uint8_t *data,
                               uint32_t len)
{
    if (!path || !data)
        return 0;
    int fd = open((const char *)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 0;
    ssize_t w = write(fd, data, len);
    close(fd);
    return (w == (ssize_t)len) ? 1 : 0;
}

static uint32_t api_read_file(const uint8_t *path, uint8_t *buf, uint32_t max)
{
    if (!path || !buf)
        return 0;
    int fd = open((const char *)path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, max);
    close(fd);
    return (n > 0) ? (uint32_t)n : 0;
}

static uint32_t api_list_directory(const uint8_t *path, uint8_t *buf,
                                   uint32_t max)
{
    /* getdents64 via the raw syscall: a NEP has no libc, so the shim does
     * the walking and renders "name\n" per entry, directories with "/". */
    struct dent { unsigned long long ino; long long off; unsigned short rlen;
                  unsigned char type; char name[]; };
    uint8_t kbuf[4096];
    if (!path || !buf)
        return 0;
    int fd = open((const char *)path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return 0;
    uint32_t total = 0;
    for (;;) {
        long n = syscall(217, fd, kbuf, sizeof kbuf);
        if (n <= 0)
            break;
        for (long off = 0; off < n;) {
            struct dent *de = (void *)(kbuf + off);
            if (de->rlen == 0)
                break;
            for (const char *c = de->name; *c && total + 2 < max; c++)
                buf[total++] = (uint8_t)*c;
            if (de->type == 4 && total + 1 < max)   /* DT_DIR */
                buf[total++] = '/';
            if (total + 1 < max)
                buf[total++] = '\n';
            off += de->rlen;
        }
    }
    close(fd);
    if (max)
        buf[total] = '\0';
    return total;
}

static void urandom(void *buf, uint32_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        for (uint32_t i = 0; i < len; i++)
            ((uint8_t *)buf)[i] = (uint8_t)(i * 131u + 7u);
        return;
    }
    uint32_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (uint8_t *)buf + got, len - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    close(fd);
}

static uint32_t api_random_u32(void)
{
    uint32_t v;
    urandom(&v, sizeof v);
    return v;
}

static uint64_t api_random_u64(void)
{
    uint64_t v;
    urandom(&v, sizeof v);
    return v;
}

static void api_random_fill(uint8_t *buf, uint32_t len)
{
    urandom(buf, len);
}

static struct KernelApi g_api = {
    api_get_version, api_create_window, api_draw_pixel, api_fill_rect,
    api_draw_text, api_draw_button, api_draw_input, api_poll_event,
    api_exit_app, api_create_button, api_create_input, api_create_label,
    api_get_input_text, api_set_ctrl_text, api_get_event, api_delete_ctrl,
    api_set_ctrl_visible, api_write_file, api_read_file, api_list_directory,
    api_random_u32, api_random_u64, api_random_fill,
};

/* ============================ DRM setup ================================== */

static int drm_setup(void)
{
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "nep1: no /dev/dri/card0: %s\n", strerror(errno));
        return -1;
    }

    drm_mode_card_res_t res;
    memset(&res, 0, sizeof res);
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 ||
        res.count_crtcs < 1 || res.count_connectors < 1) {
        fprintf(stderr, "nep1: no KMS resources\n");
        return -1;
    }

    unsigned crtc_id, conn_id, enc_id;
    res.crtc_id_ptr = (unsigned long)&crtc_id;       res.count_crtcs = 1;
    res.connector_id_ptr = (unsigned long)&conn_id; res.count_connectors = 1;
    res.encoder_id_ptr = (unsigned long)&enc_id;    res.count_encoders = 1;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        fprintf(stderr, "nep1: GETRESOURCES ids failed\n");
        return -1;
    }

    drm_mode_get_connector_t conn;
    drm_mode_modeinfo_t modes[8];
    memset(&conn, 0, sizeof conn);
    conn.modes_ptr = (unsigned long)modes;
    conn.count_modes = 8;
    unsigned enc_ids[4];
    conn.encoders_ptr = (unsigned long)enc_ids;
    conn.count_encoders = 4;
    conn.connector_id = conn_id;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0 ||
        conn.count_modes < 1 || conn.connection != 1) {
        fprintf(stderr, "nep1: connector not connected\n");
        return -1;
    }

    drm_mode_modeinfo_t *mode = &modes[0];       /* preferred mode */
    unsigned crtc_for_conn = crtc_id;            /* single-crtc hardware */

    drm_mode_create_dumb_t cr;
    memset(&cr, 0, sizeof cr);
    cr.width = mode->hdisplay;
    cr.height = mode->vdisplay;
    cr.bpp = 32;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) != 0) {
        fprintf(stderr, "nep1: CREATE_DUMB failed\n");
        return -1;
    }
    fb_w = cr.width; fb_h = cr.height; fb_pitch = cr.pitch; fb_size = cr.size;

    drm_mode_map_dumb_t mp = { .handle = cr.handle };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) != 0) {
        fprintf(stderr, "nep1: MAP_DUMB failed\n");
        return -1;
    }
    fb = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd,
              mp.offset);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "nep1: mmap of dumb buffer failed\n");
        return -1;
    }

    drm_mode_fb_cmd2_t fbcmd;
    memset(&fbcmd, 0, sizeof fbcmd);
    fbcmd.width = fb_w;
    fbcmd.height = fb_h;
    fbcmd.pixel_format = DRM_FORMAT_XRGB8888;
    fbcmd.handles[0] = cr.handle;
    fbcmd.pitches[0] = cr.pitch;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fbcmd) != 0) {
        fprintf(stderr, "nep1: ADDFB2 failed\n");
        return -1;
    }

    drm_mode_crtc_t set;
    memset(&set, 0, sizeof set);
    set.set_connectors_ptr = (unsigned long)&conn_id;
    set.count_connectors = 1;
    set.crtc_id = crtc_for_conn;
    set.fb_id = fbcmd.fb_id;
    set.mode_valid = 1;
    set.mode = *mode;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
        fprintf(stderr, "nep1: SETCRTC failed\n");
        return -1;
    }
    return 0;
}

/* ============================ loader ===================================== */

static void fail(const char *msg)
{
    fprintf(stderr, "nep1: %s\n", msg);
    exit(1);
}

static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: nep1 <program.nep>\n");
        return 2;
    }

    if (drm_setup() != 0)
        return 1;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    ev_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (ev_fd < 0)
        ev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        fail("cannot open file");
    off_t flen = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (flen < NEP1_HDR)
        fail("bad header");
    uint8_t *file = malloc((size_t)flen);
    if (!file)
        fail("out of memory");
    size_t got = 0;
    while (got < (size_t)flen) {
        ssize_t n = read(fd, file + got, (size_t)flen - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);

    uint32_t magic = (uint32_t)file[0] | ((uint32_t)file[1] << 8) |
                     ((uint32_t)file[2] << 16) | ((uint32_t)file[3] << 24);
    uint16_t version = (uint16_t)(file[4] | (file[5] << 8));
    uint16_t hdrsz = (uint16_t)(file[6] | (file[7] << 8));
    uint32_t entry_off = (uint32_t)file[8] | ((uint32_t)file[9] << 8) |
                         ((uint32_t)file[10] << 16) | ((uint32_t)file[11] << 24);
    uint32_t img_size = (uint32_t)file[12] | ((uint32_t)file[13] << 8) |
                        ((uint32_t)file[14] << 16) | ((uint32_t)file[15] << 24);

    if (magic != NEP1_MAGIC)
        fail("invalid magic");
    if (version != NEP1_VERSION)
        fail("version");
    if (hdrsz != NEP1_HDR)
        fail("bad header");
    if (entry_off >= (uint32_t)flen)
        fail("bad header");
    if (img_size == 0 || img_size > NEP1_MAXIMG)
        fail("bad image size");

    printf("nep1: %s: version=%u entry=0x%x image=0x%x\n", argv[1], version,
           entry_off, img_size);

    size_t maplen = ((size_t)img_size + 0xFFF) & ~(size_t)0xFFF;
    uint8_t *base = mmap(NULL, maplen, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        fail("mmap failed");
    size_t payload = (size_t)flen - hdrsz;
    if (payload > maplen)
        payload = maplen;
    memcpy(base, file + hdrsz, payload);
    memset(base + payload, 0, maplen - payload);
    free(file);

    fflush(stdout);
    void (*nep_main)(struct KernelApi *) =
        (void (*)(struct KernelApi *))(base + (entry_off - hdrsz));
    nep_main(&g_api);
    fflush(stdout);

    /* NeoRunst semantics: nep_main returning leaves the windows up and the
     * program alive -- the event loop drives callbacks from here on. */
    while (g_running) {
        struct pollfd pf = { .fd = ev_fd, .events = POLLIN };
        poll(&pf, 1, 200);
        pump_input();
        pump_keyboard();
    }
    printf("nep1: exit\n");
    return 0;
}
