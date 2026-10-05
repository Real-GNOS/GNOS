#include "kernel/syscall/win32.h"

#include "drivers/char/keyboard.h"
#include "drivers/char/console/io.h"
#include "kernel/gui/font.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/gui.h"
#include "kernel/gui/server.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/wm.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "lib/str/str.h"

#define W32_MAX_APP 4
#define W32_MAX_WIN 4
#define W32_MAX_DC 12
#define W32_MAX_OBJ 32
#define W32_MAX_CLS 8
#define W32_MSGQ 32
#define W32_HDC_TAG 0x00020000u
#define W32_HOBJ_TAG 0x00040000u
#define W32_HWND_TAG 0x00010000u
#define W32_FONT_PX 13
#define W32_TEXT_MAX 255
#define W32_ARG_BASE WIN_STACK_BASE

#define W32_WM_CREATE 0x0001
#define W32_WM_DESTROY 0x0002
#define W32_WM_SIZE 0x0005
#define W32_WM_PAINT 0x000F
#define W32_WM_CLOSE 0x0010
#define W32_WM_QUIT 0x0012
#define W32_WM_KEYDOWN 0x0100
#define W32_WM_KEYUP 0x0101
#define W32_WM_CHAR 0x0102
#define W32_WM_SYSKEYDOWN 0x0104
#define W32_WM_SYSKEYUP 0x0105

struct W32_OBJ {
    int used;
    int kind;
    int null_obj;
    uint32_t color;
    int width;
    struct GFX_CANVAS cv;
};

struct W32_DC {
    int used;
    struct TASK *owner;
    int win;
    int bmp;
    int pen;
    int brush;
    uint32_t text;
    uint32_t bk;
    int bk_mode;
    int curx;
    int cury;
};

struct W32_WIN {
    int used;
    int app;
    int cls;
    uint32_t id;
    char title[24];
    int x;
    int y;
    int w;
    int h;
    struct WL_CLIENT *conn;
    struct WL_SURFACE *surf;
    struct WL_SHM_POOL *pool;
    struct GFX_CANVAS cv;
    int shown;
    int invalid;
    int paint_pending;
    uint8_t kdown[128];
};

struct W32_CLS {
    int used;
    char name[32];
    uint64_t proc;
};

struct W32_APP {
    int used;
    struct TASK *owner;
    int quit;
    int quit_code;
    uint64_t msg[W32_MSGQ][4];
    int mh;
    int mt;
};

static struct W32_APP w32_apps[W32_MAX_APP];
static struct W32_WIN w32_wins[W32_MAX_WIN];
static struct W32_DC w32_dcs[W32_MAX_DC];
static struct W32_OBJ w32_objs[W32_MAX_OBJ];
static struct W32_CLS w32_clss[W32_MAX_CLS];
static int w32_stock[9];
static int w32_gui_started;

static const uint8_t w32_vk[128] = {
    [0x01] = 0x1B, [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9',
    [0x0B] = '0', [0x0C] = 0xBD, [0x0D] = 0xBB, [0x0E] = 0x08, [0x0F] = 0x09,
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = 0xDB, [0x1B] = 0xDD, [0x1C] = 0x0D, [0x1D] = 0x11, [0x1E] = 'A',
    [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = 0xBA, [0x28] = 0xDE,
    [0x29] = 0xC0, [0x2A] = 0x10, [0x2B] = 0xDC, [0x2C] = 'Z', [0x2D] = 'X',
    [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = 0xBC, [0x34] = 0xBE, [0x35] = 0xBF, [0x36] = 0x10, [0x37] = 0x6A,
    [0x38] = 0x12, [0x39] = 0x20, [0x3A] = 0x14, [0x3B] = 0x70, [0x3C] = 0x71,
    [0x3D] = 0x72, [0x3E] = 0x73, [0x3F] = 0x74, [0x40] = 0x75, [0x41] = 0x76,
    [0x42] = 0x77, [0x43] = 0x78, [0x44] = 0x79, [0x45] = 0x90, [0x46] = 0x91,
    [0x47] = 0x24, [0x48] = 0x26, [0x49] = 0x21, [0x4A] = 0x6D, [0x4B] = 0x25,
    [0x4C] = 0x6C, [0x4D] = 0x27, [0x4E] = 0x6B, [0x4F] = 0x23, [0x50] = 0x28,
    [0x51] = 0x22, [0x52] = 0x2D, [0x53] = 0x2E,
};

static const uint32_t w32_stock_col[9] = {
    0x00FFFFFFu, 0x00C0C0C0u, 0x00808080u, 0x00404040u, 0x00000000u,
    0x00FFFFFFu, 0x00FFFFFFu, 0x00000000u, 0x00000000u,
};
static const int w32_stock_kind[9] = {1, 1, 1, 1, 1, 1, 0, 0, 0};
static const int w32_stock_null[9] = {0, 0, 0, 0, 0, 1, 0, 0, 1};

static int w32_arg(struct ARCH_REGS *r, int n, uint64_t *out) {
    uint32_t off = W32_ARG_BASE + (uint32_t)(n - 5) * 8u;
    uint32_t addr = (uint32_t)r->user_rsp + off;
    if (n < 5 || !access_ok((const void *)(uintptr_t)addr, 8, 0))
        return -1;
    *out = *(const uint64_t *)(uintptr_t)addr;
    return 0;
}

static struct W32_APP *w32_app(void) {
    struct TASK *t = current;
    for (int i = 0; i < W32_MAX_APP; i++)
        if (w32_apps[i].used && w32_apps[i].owner == t)
            return &w32_apps[i];
    for (int i = 0; i < W32_MAX_APP; i++) {
        if (w32_apps[i].used)
            continue;
        memset(&w32_apps[i], 0, sizeof(w32_apps[i]));
        w32_apps[i].used = 1;
        w32_apps[i].owner = t;
        return &w32_apps[i];
    }
    return 0;
}

static int w32_app_no(struct W32_APP *a) {
    return (int)(a - w32_apps);
}

static struct W32_WIN *w32_win(uint32_t id) {
    for (int i = 0; i < W32_MAX_WIN; i++) {
        struct W32_WIN *w = &w32_wins[i];
        if (!w->used || w->id != id)
            continue;
        if (w32_apps[w->app].owner != current)
            return 0;
        return w;
    }
    return 0;
}

static struct W32_WIN *w32_win_free(void) {
    for (int i = 0; i < W32_MAX_WIN; i++)
        if (!w32_wins[i].used)
            return &w32_wins[i];
    return 0;
}

static void w32_gui_thread(void *arg) {
    (void)arg;
    gui_session_run();
}

static int w32_ensure_gui(void) {
    if (gui_session_ready() && gfx_fb_bpp() == 32)
        return 0;
    if (!w32_gui_started) {
        w32_gui_started = 1;
        kernel_thread("w32gui", 5, w32_gui_thread, 0, 0xF);
    }
    for (int i = 0; i < 800; i++) {
        if (gui_session_ready() && gfx_fb_bpp() == 32)
            return 0;
        mtime_sleep(10);
    }
    return -1;
}

static int w32_realize(struct W32_WIN *w) {
    if (w->surf)
        return 0;
    if (w->w <= 0 || w->h <= 0)
        return -1;
    if (w32_ensure_gui() != 0)
        return -1;
    if (!w->conn) {
        w->conn = wl_display_connect("win32");
        if (!w->conn)
            return -1;
    }
    uint32_t bytes = (uint32_t)w->w * (uint32_t)w->h * 4u;
    struct WL_SHM_POOL *pool = shm_pool_create(bytes);
    if (!pool)
        return -1;
    struct WL_SURFACE *s = wl_compositor_create_surface(w->conn, w->title);
    if (!s) {
        shm_pool_destroy(pool);
        return -1;
    }
    w->surf = s;
    w->pool = pool;
    memset(&w->cv, 0, sizeof(w->cv));
    w->cv.pixels = (gfx_color *)pool->data;
    w->cv.pitch = w->w * 4;
    w->cv.w = w->w;
    w->cv.h = w->h;
    w->cv.bytes = bytes;
    gfx_fill(&w->cv, 0, 0, w->cv.w, w->cv.h, GFX_RGB(0xF0, 0xF0, 0xF0));
    wl_surface_attach(s, pool, w->w, w->h);
    wl_surface_commit(s);
    wm_manage(s);
    s->x = w->x;
    s->y = w->y;
    s->w = w->w;
    s->h = w->h;
    comp_damage_surface(s);
    return 0;
}

static void w32_teardown(struct W32_WIN *w) {
    if (w->surf) {
        wm_unmanage(w->surf);
        wl_surface_destroy(w->surf);
        w->surf = 0;
    }
    if (w->pool) {
        shm_pool_destroy(w->pool);
        w->pool = 0;
    }
    if (w->conn) {
        wl_display_disconnect(w->conn);
        w->conn = 0;
    }
    memset(&w->cv, 0, sizeof(w->cv));
    w->shown = 0;
    w->invalid = 0;
    w->paint_pending = 0;
}

static void w32_commit(struct W32_WIN *w) {
    if (w->surf)
        wl_surface_commit(w->surf);
}

static struct W32_DC *w32_dc(uint32_t h) {
    if ((h & 0xFFFF0000u) != W32_HDC_TAG)
        return 0;
    uint32_t i = h & 0xFFFFu;
    if (i >= W32_MAX_DC || !w32_dcs[i].used)
        return 0;
    if (w32_dcs[i].owner != current)
        return 0;
    return &w32_dcs[i];
}

static uint32_t w32_dc_handle(struct W32_DC *d) {
    return W32_HDC_TAG + (uint32_t)(d - w32_dcs);
}

static struct W32_DC *w32_dc_alloc(int win) {
    for (int i = 0; i < W32_MAX_DC; i++) {
        if (w32_dcs[i].used)
            continue;
        struct W32_DC *d = &w32_dcs[i];
        memset(d, 0, sizeof(*d));
        d->used = 1;
        d->owner = current;
        d->win = win;
        d->bmp = -1;
        d->pen = -1;
        d->brush = -1;
        d->text = 0xFF000000u;
        d->bk = 0xFFFFFFFFu;
        d->bk_mode = 2;
        return d;
    }
    return 0;
}

static struct W32_DC *w32_dc_win(int wi) {
    for (int i = 0; i < W32_MAX_DC; i++)
        if (w32_dcs[i].used && w32_dcs[i].owner == current &&
            w32_dcs[i].win == wi)
            return &w32_dcs[i];
    return w32_dc_alloc(wi);
}

static struct GFX_CANVAS *w32_dcv(struct W32_DC *d) {
    if (d->bmp >= 0 && w32_objs[d->bmp].used)
        return &w32_objs[d->bmp].cv;
    if (d->win >= 0) {
        struct W32_WIN *w = &w32_wins[d->win];
        if (w->used && w->surf)
            return &w->cv;
    }
    return 0;
}

static struct W32_OBJ *w32_obj(uint32_t h) {
    if ((h & 0xFFFF0000u) != W32_HOBJ_TAG)
        return 0;
    uint32_t i = h & 0xFFFFu;
    if (i >= W32_MAX_OBJ || !w32_objs[i].used)
        return 0;
    return &w32_objs[i];
}

static int w32_obj_slot(void) {
    for (int i = 0; i < W32_MAX_OBJ; i++)
        if (!w32_objs[i].used)
            return i;
    return -1;
}

static uint32_t w32_obj_new(int kind, uint32_t color, int width) {
    int i = w32_obj_slot();
    if (i < 0)
        return 0;
    struct W32_OBJ *o = &w32_objs[i];
    memset(o, 0, sizeof(*o));
    o->used = 1;
    o->kind = kind;
    o->color = color;
    o->width = width;
    return W32_HOBJ_TAG + (uint32_t)i;
}

static int64_t w32_bmp_new(int w, int h) {
    int i = w32_obj_slot();
    if (i < 0)
        return 0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return 0;
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
    if (bytes > 0x7fffffffu)
        return 0;
    uint64_t pix = win_heap_alloc((uint32_t)bytes, 0);
    if (pix == 0)
        return 0;
    struct W32_OBJ *o = &w32_objs[i];
    memset(o, 0, sizeof(*o));
    o->used = 1;
    o->kind = 2;
    o->cv.pixels = (gfx_color *)(uintptr_t)pix;
    o->cv.pitch = w * 4;
    o->cv.w = w;
    o->cv.h = h;
    o->cv.bytes = (size_t)bytes;
    gfx_fill(&o->cv, 0, 0, w, h, GFX_RGB(0, 0, 0));
    return (int64_t)(W32_HOBJ_TAG + (uint32_t)i);
}

static uint32_t w32_cr(uint32_t colorref) {
    return GFX_RGB((colorref >> 0) & 0xFFu, (colorref >> 8) & 0xFFu,
                   (colorref >> 16) & 0xFFu);
}

static uint32_t w32_dc_text(struct W32_DC *d) {
    return d->text;
}

static uint32_t w32_dc_brush_col(struct W32_DC *d) {
    if (d->brush < 0)
        return 0;
    struct W32_OBJ *o = &w32_objs[d->brush];
    if (!o->used || o->null_obj)
        return 0;
    return w32_cr(o->color);
}

static uint32_t w32_dc_pen_col(struct W32_DC *d) {
    if (d->pen < 0)
        return 0;
    struct W32_OBJ *o = &w32_objs[d->pen];
    if (!o->used || o->null_obj)
        return 0;
    return w32_cr(o->color);
}

static void w32_post(struct W32_APP *a, uint32_t hwnd, uint32_t msg,
                     uint64_t wp, uint64_t lp) {
    int next = (a->mh + 1) % W32_MSGQ;
    if (next == a->mt)
        return;
    a->msg[a->mh][0] = hwnd;
    a->msg[a->mh][1] = msg;
    a->msg[a->mh][2] = wp;
    a->msg[a->mh][3] = lp;
    a->mh = next;
}

static int w32_take(struct W32_APP *a, uint64_t *m) {
    if (a->mt == a->mh)
        return 0;
    for (int k = 0; k < 4; k++)
        m[k] = a->msg[a->mt][k];
    a->mt = (a->mt + 1) % W32_MSGQ;
    return 1;
}

static int w32_pull(struct W32_WIN *w, struct WL_EVENT *ev) {
    if (!w->conn)
        return 0;
    lock_acquire(&w->conn->lock);
    if (w->conn->qtail == w->conn->qhead) {
        lock_release(&w->conn->lock);
        return 0;
    }
    *ev = w->conn->queue[w->conn->qtail];
    w->conn->qtail = (w->conn->qtail + 1) % WL_CLIENT_QUEUE;
    lock_release(&w->conn->lock);
    return 1;
}

static void w32_key_event(struct W32_APP *a, struct W32_WIN *w,
                          struct WL_EVENT *ev) {
    int sc = (int)ev->a & 0x7F;
    int vk = (int)w32_vk[sc];
    if (ev->b) {
        char ch;
        if (w->kdown[sc])
            return;
        w->kdown[sc] = 1;
        w32_post(a, w->id, vk == 0x12 ? W32_WM_SYSKEYDOWN : W32_WM_KEYDOWN,
                 (uint64_t)vk, 1);
        ch = keyboard_translate((uint8_t)sc, (int)(ev->c & KBD_MOD_SHIFT));
        if (ch == '\n')
            ch = '\r';
        if ((uint8_t)ch >= 0x20 && (uint8_t)ch < 0x7F)
            w32_post(a, w->id, W32_WM_CHAR, (uint64_t)(uint8_t)ch, 1);
        else if (ch == '\r' || ch == 0x08 || ch == '\t')
            w32_post(a, w->id, W32_WM_CHAR, (uint64_t)(uint8_t)ch, 1);
        return;
    }
    w->kdown[sc] = 0;
    w32_post(a, w->id, vk == 0x12 ? W32_WM_SYSKEYUP : W32_WM_KEYUP,
             (uint64_t)vk, 1);
}

static void w32_pump(struct W32_APP *a) {
    int ai = w32_app_no(a);
    for (int i = 0; i < W32_MAX_WIN; i++) {
        struct W32_WIN *w = &w32_wins[i];
        struct WL_EVENT ev;
        if (!w->used || w->app != ai)
            continue;
        while (w32_pull(w, &ev)) {
            switch (ev.type) {
            case WL_EV_KEY:
                w32_key_event(a, w, &ev);
                break;
            case WL_EV_CONFIGURE:
                if (ev.a > 0 && ev.b > 0) {
                    w->w = (int)ev.a;
                    w->h = (int)ev.b;
                }
                w32_post(a, w->id, W32_WM_SIZE, 0,
                         (uint64_t)((uint32_t)ev.b << 16 | (uint32_t)ev.a));
                break;
            case WL_EV_CLOSE:
                w32_post(a, w->id, W32_WM_CLOSE, 0, 0);
                break;
            default:
                break;
            }
        }
    }
}

static void w32_gen_paint(struct W32_APP *a) {
    int ai = w32_app_no(a);
    for (int i = 0; i < W32_MAX_WIN; i++) {
        struct W32_WIN *w = &w32_wins[i];
        if (!w->used || w->app != ai || !w->surf || w->paint_pending)
            continue;
        if (!w->invalid)
            continue;
        w->invalid = 0;
        w->paint_pending = 1;
        w32_post(a, w->id, W32_WM_PAINT, 0, 0);
    }
}

static void w32_store_msg(uint64_t uptr, uint32_t hwnd, uint32_t msg,
                          uint64_t wp, uint64_t lp) {
    uint8_t *p;
    if (uptr == 0 || !access_ok((const void *)(uintptr_t)uptr, 48, 1))
        return;
    p = (uint8_t *)(uintptr_t)uptr;
    memset(p, 0, 48);
    *(uint32_t *)(p + 0) = hwnd;
    *(uint32_t *)(p + 8) = msg;
    *(uint64_t *)(p + 16) = wp;
    *(uint64_t *)(p + 24) = lp;
}

static int w32_str_n(const char *s, int n, char *dst, int cap) {
    if (s == 0)
        return 0;
    if (n < 0)
        n = user_strnlen(s, (uint32_t)(cap - 1));
    if (n > cap - 1)
        n = cap - 1;
    if (n > 0) {
        if (!access_ok(s, (size_t)n, 0))
            return 0;
        copy_from_user(dst, s, (uint32_t)n);
    }
    dst[n] = 0;
    return n;
}

static int w32_utf8(const char *src, int n, char *dst, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 3; i++) {
        uint8_t c = (uint8_t)src[i];
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3Fu));
        }
    }
    dst[o] = 0;
    return o;
}

static void w32_text_out(struct W32_DC *d, struct GFX_CANVAS *cv, int x, int y,
                         const char *s, int n) {
    char raw[W32_TEXT_MAX + 1];
    char utf[(W32_TEXT_MAX + 1) * 2];
    int len = w32_str_n(s, n, raw, sizeof(raw));
    int ul = w32_utf8(raw, len, utf, (int)sizeof(utf));
    if (ul == 0)
        return;
    if (d->bk_mode == 2) {
        int tw = font_text_width(utf, W32_FONT_PX);
        gfx_fill(cv, x, y, tw, W32_FONT_PX + 2, d->bk);
    }
    font_draw(cv, x, y, utf, W32_FONT_PX, w32_dc_text(d));
}

static void w32_alpha_fix(struct GFX_CANVAS *c, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        gfx_color *row;
        int yy = y + j;
        if (yy < 0 || yy >= c->h)
            continue;
        row = gfx_row(c, yy);
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= c->w)
                continue;
            row[xx] |= 0xFF000000u;
        }
    }
}

static uint32_t w32_isqrt(uint32_t v) {
    uint32_t r = 0;
    uint32_t b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b != 0) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

int64_t w32_register_class_a(struct ARCH_REGS *r, uint64_t wc, uint64_t a1,
                             uint64_t a2, uint64_t a3) {
    char name[32];
    uint64_t proc;
    uint64_t namep;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    if (wc == 0 || !access_ok((const void *)(uintptr_t)wc, 72, 0))
        return 0;
    proc = *(const uint64_t *)(uintptr_t)(wc + 8);
    namep = *(const uint64_t *)(uintptr_t)(wc + 64);
    if (win_user_name(name, sizeof(name), namep) != 0)
        return 0;
    for (int i = 0; i < W32_MAX_CLS; i++) {
        if (!w32_clss[i].used || strcmp(w32_clss[i].name, name) != 0)
            continue;
        w32_clss[i].proc = proc;
        return 1;
    }
    for (int i = 0; i < W32_MAX_CLS; i++) {
        if (w32_clss[i].used)
            continue;
        memset(&w32_clss[i], 0, sizeof(w32_clss[i]));
        w32_clss[i].used = 1;
        strncpy(w32_clss[i].name, name, sizeof(w32_clss[i].name) - 1);
        w32_clss[i].proc = proc;
        return 1;
    }
    return 0;
}

int64_t w32_create_window_ex_a(struct ARCH_REGS *r, uint64_t exstyle,
                               uint64_t cls, uint64_t name, uint64_t style) {
    uint64_t v[8];
    char cname[32];
    struct W32_APP *a;
    struct W32_WIN *w;
    int ci = -1;
    int slot;
    (void)exstyle;
    (void)style;
    for (int i = 0; i < 8; i++)
        if (w32_arg(r, 5 + i, &v[i]) != 0)
            return 0;
    if (win_user_name(cname, sizeof(cname), cls) != 0)
        return 0;
    for (int i = 0; i < W32_MAX_CLS; i++)
        if (w32_clss[i].used && strcmp(w32_clss[i].name, cname) == 0)
            ci = i;
    if (ci < 0)
        return 0;
    a = w32_app();
    if (!a)
        return 0;
    w = w32_win_free();
    if (!w)
        return 0;
    slot = (int)(w - w32_wins);
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->app = w32_app_no(a);
    w->cls = ci;
    w->id = W32_HWND_TAG + (uint32_t)slot;
    w->x = (int)(int32_t)v[0];
    w->y = (int)(int32_t)v[1];
    w->w = (int)(int32_t)v[2];
    w->h = (int)(int32_t)v[3];
    if ((int32_t)v[0] == (int32_t)0x80000000u)
        w->x = 40 + slot * 24;
    if ((int32_t)v[1] == (int32_t)0x80000000u)
        w->y = 40 + slot * 24;
    if (w->w < 1 || w->w > 2048)
        w->w = 420;
    if (w->h < 1 || w->h > 2048)
        w->h = 240;
    strcpy(w->title, "win32");
    win_user_name(w->title, sizeof(w->title), name);
    return (int64_t)w->id;
}

int64_t w32_show_window(struct ARCH_REGS *r, uint64_t hwnd, uint64_t cmd,
                        uint64_t a2, uint64_t a3) {
    struct W32_WIN *w;
    (void)r;
    (void)a2;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    if (w32_realize(w) != 0)
        return 0;
    w->shown = 1;
    w->invalid = 1;
    w32_commit(w);
    if ((int)cmd == 0)
        return 1;
    return 0;
}

int64_t w32_update_window(struct ARCH_REGS *r, uint64_t hwnd, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    struct W32_WIN *w;
    struct W32_APP *a;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    w32_commit(w);
    a = &w32_apps[w->app];
    if (w->invalid && !w->paint_pending) {
        w->invalid = 0;
        w->paint_pending = 1;
        w32_post(a, w->id, W32_WM_PAINT, 0, 0);
    }
    return 1;
}

int64_t w32_get_message_a(struct ARCH_REGS *r, uint64_t msg, uint64_t hwnd,
                          uint64_t lo, uint64_t hi) {
    struct W32_APP *a;
    uint64_t m[4];
    (void)r;
    (void)hwnd;
    (void)lo;
    (void)hi;
    a = w32_app();
    if (!a)
        return 0;
    for (;;) {
        w32_pump(a);
        w32_gen_paint(a);
        if (a->quit) {
            w32_store_msg(msg, 0, W32_WM_QUIT, (uint64_t)(uint32_t)a->quit_code,
                          0);
            return 0;
        }
        if (w32_take(a, m)) {
            w32_store_msg(msg, (uint32_t)m[0], (uint32_t)m[1], m[2], m[3]);
            return 1;
        }
        mtime_sleep(5);
    }
}

int64_t w32_peek_message_a(struct ARCH_REGS *r, uint64_t msg, uint64_t hwnd,
                           uint64_t lo, uint64_t hi) {
    struct W32_APP *a;
    uint64_t m[4];
    uint64_t rm;
    (void)hwnd;
    (void)lo;
    (void)hi;
    if (w32_arg(r, 5, &rm) != 0)
        rm = 1;
    a = w32_app();
    if (!a)
        return 0;
    w32_pump(a);
    w32_gen_paint(a);
    if (a->mt == a->mh)
        return 0;
    for (int k = 0; k < 4; k++)
        m[k] = a->msg[a->mt][k];
    if (rm != 0)
        w32_take(a, m);
    if ((uint32_t)m[1] == W32_WM_QUIT)
        return 0;
    w32_store_msg(msg, (uint32_t)m[0], (uint32_t)m[1], m[2], m[3]);
    return 1;
}

int64_t w32_translate_message(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

int64_t w32_dispatch_message_a(struct ARCH_REGS *r, uint64_t msg, uint64_t a1,
                               uint64_t a2, uint64_t a3) {
    struct W32_WIN *w;
    uint32_t hwnd;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    if (msg == 0 || !access_ok((const void *)(uintptr_t)msg, 32, 0))
        return 0;
    hwnd = *(const uint32_t *)(uintptr_t)msg;
    w = w32_win(hwnd);
    if (!w)
        return 0;
    return (int64_t)w32_clss[w->cls].proc;
}

int64_t w32_def_window_proc_a(struct ARCH_REGS *r, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

int64_t w32_post_quit_message(struct ARCH_REGS *r, uint64_t code, uint64_t a1,
                              uint64_t a2, uint64_t a3) {
    struct W32_APP *a;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    a = w32_app();
    if (!a)
        return 0;
    a->quit = 1;
    a->quit_code = (int)(int32_t)code;
    return 0;
}

int64_t w32_post_message_a(struct ARCH_REGS *r, uint64_t hwnd, uint64_t msg,
                           uint64_t wp, uint64_t lp) {
    struct W32_WIN *w;
    (void)r;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    w32_post(&w32_apps[w->app], w->id, (uint32_t)msg, wp, lp);
    return 1;
}

int64_t w32_destroy_window(struct ARCH_REGS *r, uint64_t hwnd, uint64_t a1,
                           uint64_t a2, uint64_t a3) {
    struct W32_WIN *w;
    uint32_t id;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    id = w->id;
    w32_teardown(w);
    w32_post(&w32_apps[w->app], id, W32_WM_DESTROY, 0, 0);
    w->used = 0;
    return 1;
}

int64_t w32_get_dc(struct ARCH_REGS *r, uint64_t hwnd, uint64_t a1, uint64_t a2,
                   uint64_t a3) {
    struct W32_WIN *w;
    struct W32_DC *d;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    if (w32_realize(w) != 0)
        return 0;
    d = w32_dc_win((int)(w - w32_wins));
    if (!d)
        return 0;
    return (int64_t)w32_dc_handle(d);
}

int64_t w32_release_dc(struct ARCH_REGS *r, uint64_t hwnd, uint64_t hdc,
                       uint64_t a2, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    (void)r;
    (void)hwnd;
    (void)a2;
    (void)a3;
    if (!d || d->win < 0)
        return 0;
    w32_commit(&w32_wins[d->win]);
    d->used = 0;
    return 1;
}

int64_t w32_begin_paint(struct ARCH_REGS *r, uint64_t hwnd, uint64_t ps,
                        uint64_t a2, uint64_t a3) {
    struct W32_WIN *w;
    struct W32_DC *d;
    (void)r;
    (void)a2;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    if (w32_realize(w) != 0)
        return 0;
    d = w32_dc_win((int)(w - w32_wins));
    if (!d)
        return 0;
    w->invalid = 0;
    w->paint_pending = 0;
    if (ps != 0 && access_ok((const void *)(uintptr_t)ps, 72, 1)) {
        uint8_t *p = (uint8_t *)(uintptr_t)ps;
        memset(p, 0, 72);
        *(uint32_t *)(p + 0) = w32_dc_handle(d);
        *(uint32_t *)(p + 8) = 0;
        *(int32_t *)(p + 12) = 0;
        *(int32_t *)(p + 16) = 0;
        *(int32_t *)(p + 20) = w->w;
        *(int32_t *)(p + 24) = w->h;
    }
    return (int64_t)w32_dc_handle(d);
}

int64_t w32_end_paint(struct ARCH_REGS *r, uint64_t hwnd, uint64_t ps,
                      uint64_t a2, uint64_t a3) {
    struct W32_DC *d;
    (void)r;
    (void)a2;
    (void)a3;
    (void)ps;
    if (ps != 0 && access_ok((const void *)(uintptr_t)ps, 12, 0))
        d = w32_dc(*(const uint32_t *)(uintptr_t)ps);
    else
        d = 0;
    if (d && d->win >= 0) {
        w32_commit(&w32_wins[d->win]);
        d->used = 0;
        return 1;
    }
    (void)hwnd;
    return 1;
}

int64_t w32_get_client_rect(struct ARCH_REGS *r, uint64_t hwnd, uint64_t rc,
                            uint64_t a2, uint64_t a3) {
    struct W32_WIN *w;
    (void)r;
    (void)a2;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    if (rc == 0 || !access_ok((const void *)(uintptr_t)rc, 16, 1))
        return 0;
    *(int32_t *)(uintptr_t)(rc + 0) = 0;
    *(int32_t *)(uintptr_t)(rc + 4) = 0;
    *(int32_t *)(uintptr_t)(rc + 8) = w->w;
    *(int32_t *)(uintptr_t)(rc + 12) = w->h;
    return 1;
}

int64_t w32_invalidate_rect(struct ARCH_REGS *r, uint64_t hwnd, uint64_t rc,
                            uint64_t erase, uint64_t a3) {
    struct W32_WIN *w;
    (void)r;
    (void)rc;
    (void)erase;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    w->invalid = 1;
    return 1;
}

int64_t w32_load_cursor_a(struct ARCH_REGS *r, uint64_t inst, uint64_t name,
                          uint64_t a2, uint64_t a3) {
    (void)r;
    (void)inst;
    (void)a2;
    (void)a3;
    if (name <= 0xFFFFu)
        return (int64_t)name;
    return 1;
}

int64_t w32_load_icon_a(struct ARCH_REGS *r, uint64_t inst, uint64_t name,
                        uint64_t a2, uint64_t a3) {
    (void)r;
    (void)inst;
    (void)a2;
    (void)a3;
    if (name <= 0xFFFFu)
        return (int64_t)name;
    return 1;
}

int64_t w32_message_box_a(struct ARCH_REGS *r, uint64_t hwnd, uint64_t text,
                          uint64_t caption, uint64_t type) {
    char kt[128];
    char kc[64];
    (void)r;
    (void)hwnd;
    (void)type;
    win_user_name(kt, sizeof(kt), text);
    win_user_name(kc, sizeof(kc), caption);
    kprintf("win32: MessageBox [%s] %s\n", kc, kt);
    return 1;
}

int64_t w32_set_window_text_a(struct ARCH_REGS *r, uint64_t hwnd, uint64_t text,
                              uint64_t a2, uint64_t a3) {
    struct W32_WIN *w;
    (void)r;
    (void)a2;
    (void)a3;
    w = w32_win((uint32_t)hwnd);
    if (!w)
        return 0;
    strcpy(w->title, "win32");
    win_user_name(w->title, sizeof(w->title), text);
    if (w->surf) {
        strncpy(w->surf->title, w->title, 23);
        w->surf->title[23] = 0;
        comp_damage_surface(w->surf);
    }
    return 1;
}

int64_t w32_get_system_metrics(struct ARCH_REGS *r, uint64_t index, uint64_t a1,
                               uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    if ((int)index == 0)
        return (int64_t)comp_screen_w();
    if ((int)index == 1)
        return (int64_t)comp_screen_h();
    if ((int)index == 4)
        return COMP_TITLE_H;
    if ((int)index == 15)
        return COMP_BORDER;
    return 0;
}

int64_t w32_message_beep(struct ARCH_REGS *r, uint64_t type, uint64_t a1,
                         uint64_t a2, uint64_t a3) {
    (void)r;
    (void)type;
    (void)a1;
    (void)a2;
    (void)a3;
    return 1;
}

int64_t w32_create_compatible_dc(struct ARCH_REGS *r, uint64_t hdc, uint64_t a1,
                                 uint64_t a2, uint64_t a3) {
    struct W32_DC *d;
    (void)r;
    (void)hdc;
    (void)a1;
    (void)a2;
    (void)a3;
    d = w32_dc_alloc(-1);
    if (!d)
        return 0;
    return (int64_t)w32_dc_handle(d);
}

int64_t w32_create_compatible_bitmap(struct ARCH_REGS *r, uint64_t hdc,
                                     uint64_t w, uint64_t h, uint64_t a3) {
    (void)r;
    (void)hdc;
    (void)a3;
    return w32_bmp_new((int)(int32_t)w, (int)(int32_t)h);
}

int64_t w32_create_dib_section(struct ARCH_REGS *r, uint64_t hdc, uint64_t pbmi,
                               uint64_t usage, uint64_t ppv) {
    int32_t bw;
    int32_t bh;
    int64_t h;
    (void)r;
    (void)hdc;
    (void)usage;
    if (pbmi == 0 || !access_ok((const void *)(uintptr_t)pbmi, 40, 0))
        return 0;
    bw = *(const int32_t *)(uintptr_t)(pbmi + 4);
    bh = *(const int32_t *)(uintptr_t)(pbmi + 8);
    if (bh < 0)
        bh = -bh;
    h = w32_bmp_new((int)bw, (int)bh);
    if (h == 0)
        return 0;
    if (ppv != 0 && access_ok((const void *)(uintptr_t)ppv, 8, 1)) {
        struct W32_OBJ *o = w32_obj((uint32_t)h);
        *(uint64_t *)(uintptr_t)ppv = (uint64_t)(uintptr_t)o->cv.pixels;
    }
    return h;
}

int64_t w32_select_object(struct ARCH_REGS *r, uint64_t hdc, uint64_t obj,
                          uint64_t a2, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct W32_OBJ *o = w32_obj((uint32_t)obj);
    uint32_t prev = 0;
    (void)r;
    (void)a2;
    (void)a3;
    if (!d || !o)
        return 0;
    if (o->kind == 2) {
        prev = (d->bmp >= 0) ? (W32_HOBJ_TAG + (uint32_t)d->bmp) : 0;
        d->bmp = (int)(o - w32_objs);
        return (int64_t)prev;
    }
    if (o->kind == 0) {
        prev = (d->pen >= 0) ? (W32_HOBJ_TAG + (uint32_t)d->pen) : 0;
        d->pen = (int)(o - w32_objs);
        return (int64_t)prev;
    }
    prev = (d->brush >= 0) ? (W32_HOBJ_TAG + (uint32_t)d->brush) : 0;
    d->brush = (int)(o - w32_objs);
    return (int64_t)prev;
}

int64_t w32_delete_object(struct ARCH_REGS *r, uint64_t obj, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    int i;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    if ((obj & 0xFFFF0000u) != W32_HOBJ_TAG)
        return 0;
    i = (int)(obj & 0xFFFFu);
    if (i >= W32_MAX_OBJ || !w32_objs[i].used)
        return 0;
    for (int k = 0; k < W32_MAX_DC; k++) {
        if (w32_dcs[k].used && w32_dcs[k].bmp == i)
            w32_dcs[k].bmp = -1;
        if (w32_dcs[k].used && w32_dcs[k].pen == i)
            w32_dcs[k].pen = -1;
        if (w32_dcs[k].used && w32_dcs[k].brush == i)
            w32_dcs[k].brush = -1;
    }
    w32_objs[i].used = 0;
    return 1;
}

int64_t w32_delete_dc(struct ARCH_REGS *r, uint64_t hdc, uint64_t a1,
                      uint64_t a2, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    if (!d)
        return 0;
    d->used = 0;
    return 1;
}

int64_t w32_bit_blt(struct ARCH_REGS *r, uint64_t dst, uint64_t x, uint64_t y,
                    uint64_t cx) {
    uint64_t v[5];
    struct W32_DC *dd = w32_dc((uint32_t)dst);
    struct W32_DC *ds;
    struct GFX_CANVAS *cd;
    struct GFX_CANVAS *cs;
    int dx = (int)(int32_t)x;
    int dy = (int)(int32_t)y;
    int dw = (int)(int32_t)cx;
    int dh;
    int sx;
    int sy;
    int rop;
    (void)r;
    for (int i = 0; i < 5; i++)
        if (w32_arg(r, 5 + i, &v[i]) != 0)
            return 0;
    dh = (int)(int32_t)v[0];
    ds = w32_dc((uint32_t)v[1]);
    sx = (int)(int32_t)v[2];
    sy = (int)(int32_t)v[3];
    rop = (int)(int32_t)v[4];
    if (!dd || !ds)
        return 0;
    if (rop != 0x00CC0020 && rop != 0x00CC0020u)
        return 0;
    cd = w32_dcv(dd);
    cs = w32_dcv(ds);
    if (!cd || !cs)
        return 0;
    gfx_blit(cd, dx, dy, cs, sx, sy, dw, dh);
    if (dd->bmp < 0)
        w32_alpha_fix(cd, dx, dy, dw, dh);
    return 1;
}

int64_t w32_stretch_blt(struct ARCH_REGS *r, uint64_t dst, uint64_t x,
                        uint64_t y, uint64_t w) {
    uint64_t v[7];
    struct W32_DC *dd = w32_dc((uint32_t)dst);
    struct W32_DC *ds;
    struct GFX_CANVAS *cd;
    struct GFX_CANVAS *cs;
    int dx = (int)(int32_t)x;
    int dy = (int)(int32_t)y;
    int dw = (int)(int32_t)w;
    int dh;
    int sx;
    int sy;
    int sw;
    int sh;
    int rop;
    (void)r;
    for (int i = 0; i < 7; i++)
        if (w32_arg(r, 5 + i, &v[i]) != 0)
            return 0;
    dh = (int)(int32_t)v[0];
    ds = w32_dc((uint32_t)v[1]);
    sx = (int)(int32_t)v[2];
    sy = (int)(int32_t)v[3];
    sw = (int)(int32_t)v[4];
    sh = (int)(int32_t)v[5];
    rop = (int)(int32_t)v[6];
    if (!dd || !ds || rop != 0x00CC0020)
        return 0;
    cd = w32_dcv(dd);
    cs = w32_dcv(ds);
    if (!cd || !cs)
        return 0;
    gfx_blit_scale(cd, dx, dy, dw, dh, cs, sx, sy, sw, sh);
    if (dd->bmp < 0)
        w32_alpha_fix(cd, dx, dy, dw, dh);
    return 1;
}

int64_t w32_pat_blt(struct ARCH_REGS *r, uint64_t hdc, uint64_t x, uint64_t y,
                    uint64_t w) {
    uint64_t v[2];
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct GFX_CANVAS *cv;
    (void)r;
    for (int i = 0; i < 2; i++)
        if (w32_arg(r, 5 + i, &v[i]) != 0)
            return 0;
    if (!d)
        return 0;
    cv = w32_dcv(d);
    if (!cv)
        return 0;
    if ((int)(int32_t)v[1] != 0x00F00021)
        return 0;
    gfx_fill(cv, (int)(int32_t)x, (int)(int32_t)y, (int)(int32_t)w,
             (int)(int32_t)v[0], w32_dc_brush_col(d));
    return 1;
}

int64_t w32_rectangle(struct ARCH_REGS *r, uint64_t hdc, uint64_t l, uint64_t t,
                      uint64_t rr) {
    uint64_t b;
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct GFX_CANVAS *cv;
    int x0 = (int)(int32_t)l;
    int y0 = (int)(int32_t)t;
    int x1;
    uint32_t bc;
    (void)r;
    if (w32_arg(r, 5, &b) != 0)
        return 0;
    if (!d)
        return 0;
    cv = w32_dcv(d);
    if (!cv)
        return 0;
    x1 = (int)(int32_t)rr;
    if (x1 < x0) {
        int tmp = x0;
        x0 = x1;
        x1 = tmp;
    }
    if ((int)(int32_t)b < y0) {
        int tmp = y0;
        y0 = (int)(int32_t)b;
        b = (uint64_t)(uint32_t)tmp;
    }
    bc = w32_dc_brush_col(d);
    if (bc != 0)
        gfx_fill(cv, x0, y0, x1 - x0, (int)(int32_t)b - y0, bc);
    gfx_rect(cv, x0, y0, x1 - x0, (int)(int32_t)b - y0, w32_dc_pen_col(d));
    return 1;
}

int64_t w32_ellipse(struct ARCH_REGS *r, uint64_t hdc, uint64_t l, uint64_t t,
                    uint64_t rr) {
    uint64_t b;
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct GFX_CANVAS *cv;
    int x0 = (int)(int32_t)l;
    int y0 = (int)(int32_t)t;
    int x1;
    int y1;
    int cx;
    int cy;
    int rx;
    int ry;
    uint32_t bc;
    uint32_t pc;
    (void)r;
    if (w32_arg(r, 5, &b) != 0)
        return 0;
    if (!d)
        return 0;
    cv = w32_dcv(d);
    if (!cv)
        return 0;
    x1 = (int)(int32_t)rr;
    y1 = (int)(int32_t)b;
    if (x1 < x0) {
        int tmp = x0;
        x0 = x1;
        x1 = tmp;
    }
    if (y1 < y0) {
        int tmp = y0;
        y0 = y1;
        y1 = tmp;
    }
    cx = (x0 + x1) / 2;
    cy = (y0 + y1) / 2;
    rx = (x1 - x0) / 2;
    ry = (y1 - y0) / 2;
    if (rx < 1 || ry < 1)
        return 1;
    bc = w32_dc_brush_col(d);
    pc = w32_dc_pen_col(d);
    for (int j = -ry; j <= ry; j++) {
        int64_t t2 = (int64_t)ry * ry - (int64_t)j * j;
        int dx = (int)((int64_t)rx * (int64_t)w32_isqrt((uint32_t)t2) / ry);
        if (bc != 0)
            gfx_hline(cv, cx - dx, cy + j, 2 * dx + 1, bc);
        if (pc != 0) {
            gfx_px(cv, cx - dx, cy + j, pc);
            gfx_px(cv, cx + dx, cy + j, pc);
        }
    }
    if (pc != 0) {
        gfx_hline(cv, cx - rx, cy, 2 * rx + 1, pc);
    }
    return 1;
}

int64_t w32_move_to_ex(struct ARCH_REGS *r, uint64_t hdc, uint64_t x, uint64_t y,
                       uint64_t pt) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    (void)r;
    if (!d)
        return 0;
    if (pt != 0 && access_ok((const void *)(uintptr_t)pt, 8, 1)) {
        *(int32_t *)(uintptr_t)(pt + 0) = d->curx;
        *(int32_t *)(uintptr_t)(pt + 4) = d->cury;
    }
    d->curx = (int)(int32_t)x;
    d->cury = (int)(int32_t)y;
    return 1;
}

int64_t w32_line_to(struct ARCH_REGS *r, uint64_t hdc, uint64_t x, uint64_t y,
                    uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct GFX_CANVAS *cv;
    int x1;
    int y1;
    uint32_t pc;
    (void)r;
    (void)a3;
    if (!d)
        return 0;
    cv = w32_dcv(d);
    if (!cv)
        return 0;
    pc = w32_dc_pen_col(d);
    if (pc == 0)
        return 0;
    x1 = (int)(int32_t)x;
    y1 = (int)(int32_t)y;
    int x0 = d->curx;
    int y0 = d->cury;
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx);
    if ((dy < 0 ? -dy : dy) > steps)
        steps = (dy < 0 ? -dy : dy);
    if (steps <= 0) {
        gfx_px(cv, x0, y0, pc);
    } else {
        for (int i = 0; i <= steps; i++) {
            int px = x0 + (int)((int64_t)dx * i / steps);
            int py = y0 + (int)((int64_t)dy * i / steps);
            gfx_px(cv, px, py, pc);
        }
    }
    d->curx = x1;
    d->cury = y1;
    return 1;
}

int64_t w32_text_out_a(struct ARCH_REGS *r, uint64_t hdc, uint64_t x,
                       uint64_t y, uint64_t s) {
    uint64_t n;
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct GFX_CANVAS *cv;
    (void)r;
    if (w32_arg(r, 5, &n) != 0)
        return 0;
    if (!d)
        return 0;
    cv = w32_dcv(d);
    if (!cv)
        return 0;
    w32_text_out(d, cv, (int)(int32_t)x, (int)(int32_t)y,
                 (const char *)(uintptr_t)s, (int)(int32_t)n);
    return 1;
}

int64_t w32_draw_text_a(struct ARCH_REGS *r, uint64_t hdc, uint64_t s,
                        uint64_t n, uint64_t rc) {
    uint64_t fmt;
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct GFX_CANVAS *cv;
    char raw[W32_TEXT_MAX + 1];
    char utf[(W32_TEXT_MAX + 1) * 2];
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int ul;
    int tw;
    (void)r;
    if (w32_arg(r, 5, &fmt) != 0)
        return 0;
    if (!d)
        return 0;
    cv = w32_dcv(d);
    if (!cv)
        return 0;
    if (rc != 0 && access_ok((const void *)(uintptr_t)rc, 16, 0)) {
        x0 = *(const int32_t *)(uintptr_t)(rc + 0);
        y0 = *(const int32_t *)(uintptr_t)(rc + 4);
        x1 = *(const int32_t *)(uintptr_t)(rc + 8);
        y1 = *(const int32_t *)(uintptr_t)(rc + 12);
    }
    int len = w32_str_n((const char *)(uintptr_t)s, (int)(int32_t)n, raw,
                        sizeof(raw));
    ul = w32_utf8(raw, len, utf, (int)sizeof(utf));
    if (ul == 0)
        return 1;
    tw = font_text_width(utf, W32_FONT_PX);
    if ((int)fmt & 1)
        x0 += (x1 - x0 - tw) / 2;
    if ((int)fmt & 0x20) {
        if ((int)fmt & 4)
            y0 += (y1 - y0 - W32_FONT_PX) / 2;
    } else {
        y0 += (y1 - y0 - W32_FONT_PX) / 2;
    }
    w32_text_out(d, cv, (int)x0, (int)y0, (const char *)(uintptr_t)s,
                 (int)(int32_t)n);
    return 1;
}

int64_t w32_set_text_color(struct ARCH_REGS *r, uint64_t hdc, uint64_t color,
                           uint64_t a2, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    uint32_t old;
    (void)r;
    (void)a2;
    (void)a3;
    if (!d)
        return 0;
    old = d->text;
    d->text = w32_cr((uint32_t)color);
    return (int64_t)old;
}

int64_t w32_set_bk_color(struct ARCH_REGS *r, uint64_t hdc, uint64_t color,
                         uint64_t a2, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    uint32_t old;
    (void)r;
    (void)a2;
    (void)a3;
    if (!d)
        return 0;
    old = d->bk;
    d->bk = w32_cr((uint32_t)color);
    return (int64_t)old;
}

int64_t w32_set_bk_mode(struct ARCH_REGS *r, uint64_t hdc, uint64_t mode,
                        uint64_t a2, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    int old;
    (void)r;
    (void)a2;
    (void)a3;
    if (!d)
        return 0;
    old = d->bk_mode;
    d->bk_mode = (int)(int32_t)mode;
    return (int64_t)old;
}

int64_t w32_create_solid_brush(struct ARCH_REGS *r, uint64_t color, uint64_t a1,
                               uint64_t a2, uint64_t a3) {
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    return (int64_t)w32_obj_new(1, (uint32_t)color, 1);
}

int64_t w32_create_pen(struct ARCH_REGS *r, uint64_t style, uint64_t width,
                       uint64_t color, uint64_t a3) {
    (void)r;
    (void)a3;
    if ((int32_t)style != 0)
        return 0;
    return (int64_t)w32_obj_new(0, (uint32_t)color, (int)(int32_t)width);
}

int64_t w32_get_stock_object(struct ARCH_REGS *r, uint64_t index, uint64_t a1,
                             uint64_t a2, uint64_t a3) {
    int i;
    (void)r;
    (void)a1;
    (void)a2;
    (void)a3;
    i = (int)(int32_t)index;
    if (i < 0 || i > 8)
        return 0;
    if (w32_stock[i] == 0) {
        uint32_t h = w32_obj_new(w32_stock_kind[i], w32_stock_col[i], 1);
        struct W32_OBJ *o = w32_obj(h);
        if (!o)
            return 0;
        o->null_obj = w32_stock_null[i];
        w32_stock[i] = (int)(h & 0xFFFFu);
    }
    return (int64_t)(W32_HOBJ_TAG + (uint32_t)w32_stock[i]);
}

int64_t w32_fill_rect(struct ARCH_REGS *r, uint64_t hdc, uint64_t rc,
                      uint64_t hbr, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct W32_OBJ *o = w32_obj((uint32_t)hbr);
    struct GFX_CANVAS *cv;
    (void)r;
    (void)a3;
    if (!d || !o)
        return 0;
    cv = w32_dcv(d);
    if (!cv || rc == 0 || !access_ok((const void *)(uintptr_t)rc, 16, 0))
        return 0;
    gfx_fill(cv, *(const int32_t *)(uintptr_t)(rc + 0),
             *(const int32_t *)(uintptr_t)(rc + 4),
             *(const int32_t *)(uintptr_t)(rc + 8) -
                 *(const int32_t *)(uintptr_t)(rc + 0),
             *(const int32_t *)(uintptr_t)(rc + 12) -
                 *(const int32_t *)(uintptr_t)(rc + 4),
             w32_cr(o->color));
    return 1;
}

int64_t w32_frame_rect(struct ARCH_REGS *r, uint64_t hdc, uint64_t rc,
                       uint64_t hbr, uint64_t a3) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct W32_OBJ *o = w32_obj((uint32_t)hbr);
    struct GFX_CANVAS *cv;
    int x;
    int y;
    int w;
    int h;
    uint32_t c;
    (void)r;
    (void)a3;
    if (!d || !o)
        return 0;
    cv = w32_dcv(d);
    if (!cv || rc == 0 || !access_ok((const void *)(uintptr_t)rc, 16, 0))
        return 0;
    x = *(const int32_t *)(uintptr_t)(rc + 0);
    y = *(const int32_t *)(uintptr_t)(rc + 4);
    w = *(const int32_t *)(uintptr_t)(rc + 8) - x;
    h = *(const int32_t *)(uintptr_t)(rc + 12) - y;
    c = w32_cr(o->color);
    gfx_hline(cv, x, y, w, c);
    gfx_hline(cv, x, y + h - 1, w, c);
    gfx_vline(cv, x, y, h, c);
    gfx_vline(cv, x + w - 1, y, h, c);
    return 1;
}

int64_t w32_set_pixel(struct ARCH_REGS *r, uint64_t hdc, uint64_t x, uint64_t y,
                      uint64_t color) {
    struct W32_DC *d = w32_dc((uint32_t)hdc);
    struct GFX_CANVAS *cv;
    gfx_color c;
    (void)r;
    if (!d)
        return 0;
    cv = w32_dcv(d);
    if (!cv)
        return 0;
    c = w32_cr((uint32_t)color);
    gfx_px(cv, (int)(int32_t)x, (int)(int32_t)y, c);
    return (int64_t)c;
}

int64_t w32_get_device_caps(struct ARCH_REGS *r, uint64_t hdc, uint64_t index,
                            uint64_t a2, uint64_t a3) {
    (void)r;
    (void)hdc;
    (void)a2;
    (void)a3;
    switch ((int)(int32_t)index) {
    case 8:
        return (int64_t)comp_screen_w();
    case 10:
        return (int64_t)comp_screen_h();
    case 12:
        return 32;
    case 14:
        return 1;
    case 24:
        return 0xFFFFFFFFll;
    case 38:
        return 256;
    case 40:
        return 0;
    default:
        break;
    }
    return 0;
}