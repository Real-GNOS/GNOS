#ifndef GUI_SERVER_H
#define GUI_SERVER_H

#include "kernel/sched/sync.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/theme.h"
#include "drivers/char/keyboard.h"
#include <stdint.h>

struct WL_SHM_POOL;

#define WL_MAX_CLIENTS 8
#define WL_MAX_SURFACES 12
#define WL_CLIENT_QUEUE 16
#define WL_MAX_WS 4

enum WL_EVENT_TYPE {
    WL_EV_NONE = 0,
    WL_EV_CONFIGURE,
    WL_EV_FRAME,
    WL_EV_KEY,
    WL_EV_CLOSE
};

struct WL_EVENT {
    int type;
    int32_t a, b, c;
};

#define MOD_SHIFT KBD_MOD_SHIFT
#define MOD_CTRL KBD_MOD_CTRL
#define MOD_ALT KBD_MOD_ALT

struct WL_CLIENT {
    int used;
    char name[16];
    struct WL_EVENT queue[WL_CLIENT_QUEUE];
    int qhead, qtail;
    struct SCHED_SEMAPHORE sema;
    struct SCHED_LOCK lock;
    struct WL_SURFACE *surf;
};

struct WL_SURFACE {
    int used;
    struct WL_CLIENT *client;
    char title[24];

    int x, y, w, h;
    int ws;
    int floating;
    uint8_t alpha;
    uint8_t *buf;
    int buf_w, buf_h;
    int frame_pending;
    void *x11_owner;
    uint32_t x11_xid;

    struct GFX_CANVAS bs;
    uint8_t bs_frame_valid;
    uint8_t bs_content_dirty;
};

struct WL_CLIENT *wl_display_connect(const char *name);
void wl_display_disconnect(struct WL_CLIENT *c);
struct WL_SURFACE *wl_compositor_create_surface(struct WL_CLIENT *c,
                                                const char *title);
int wl_surface_attach(struct WL_SURFACE *s, struct WL_SHM_POOL *pool, int w,
                      int h);
void wl_surface_commit(struct WL_SURFACE *s);
void wl_surface_destroy(struct WL_SURFACE *s);
int wl_display_dispatch(struct WL_CLIENT *c, struct WL_EVENT *ev);

void comp_init(void);
void comp_run(void);
void comp_request_exit(void);
void comp_damage_rect(int x, int y, int w, int h);
void comp_damage_surface(struct WL_SURFACE *s);

void comp_set_wallpaper(const uint32_t *pixels, int w, int h);
void comp_damage_content(struct WL_SURFACE *s);
void comp_surface_invalidate(struct WL_SURFACE *s);
void comp_invalidate_all(void);
void comp_post_key(uint8_t scancode, int pressed, uint8_t mods);
void comp_post_mouse(int dx, int dy, uint8_t buttons);
void comp_log(const char *s);
void comp_lock_acquire(void);
void comp_lock_release(void);

struct WL_SURFACE **comp_surfaces(int *count);
int comp_screen_w(void);
int comp_screen_h(void);
void comp_send_configure(struct WL_SURFACE *s, int w, int h);
void comp_send_close(struct WL_SURFACE *s);
void comp_send_key(struct WL_SURFACE *s, int scancode, int pressed, int mods);
void comp_destroy_surface_pool(struct WL_SURFACE *s, struct WL_SHM_POOL **pool);
int comp_pointer_x(void);
int comp_pointer_y(void);
uint8_t comp_pointer_buttons(void);
void wl_surface_set_alpha(struct WL_SURFACE *s, int alpha);

#define COMP_BAR_H 30
#define COMP_TITLE_H 26
#define COMP_BORDER 1

#define WIN_RADIUS 10
#define WIN_RESIZE_GRAB 5

#endif
