#include "kernel/gui/server.h"

#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "kernel/init/pit/pit.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/gui/font.h"
#include "kernel/gui/gpu.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/wm.h"
#include "kernel/gui/display.h"
#include "kernel/gui/input.h"
#include "kernel/gui/x11.h"

static struct WL_CLIENT clients[WL_MAX_CLIENTS];
static struct WL_SURFACE surfaces[WL_MAX_SURFACES];
static struct GFX_CANVAS bs_slots[WL_MAX_SURFACES];

static struct GFX_CANVAS *dst;
static int scrnx, scrny;

static struct SCHED_LOCK comp_lock;
static int session_active = 1;

#define MAX_DAMAGE 64

struct GUI_DAMAGE {
    struct GFX_RECT r;
    uint8_t content_only;
};

static struct GUI_DAMAGE damage[MAX_DAMAGE];
static int damage_n = 0;
static int damage_full = 0;

static int cur_x = 512, cur_y = 384;
static uint8_t last_buttons = 0;

#define CURSOR_W 12
#define CURSOR_H 18
static const char *cursor_bmp[CURSOR_H] = {
    "#...........", "##..........", "#O#.........", "#OO#........",
    "#OOO#.......", "#OOOO#......", "#OOOOO#.....", "#OOOOOO#....",
    "#OOOOOOO#...", "#OOOOOOOO#..", "#OOOOO#####.", "#OO#OO#.....",
    "#O#.#OO#....", "##..#OO#....", "#....#OO#...", ".....#OO#...",
    "......#OO#..", ".......##...",
};
#define CURSOR_OUTLINE GFX_RGB(0, 0, 0)
#define CURSOR_FILL GFX_RGB(255, 255, 255)
#define TITLE_FONT_PX 13
#define CLOSE_FONT_PX 10

static uint64_t perf_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t perf_repaints;
static uint64_t perf_repaint_tsc;
static uint32_t perf_loops, perf_idle_loops;
static uint32_t perf_key_events, perf_mouse_events;
static uint32_t perf_commits;
static uint32_t perf_sec_mark;

static void perf_report(void) {
    uint32_t sec = tick / PIT_HZ;
    if (sec < perf_sec_mark + 5)
        return;
    kprintf("gui: t=%us fps=%u loops=%u idle=%u key=%u mouse=%u commit=%u "
            "repaint=%uus\n",
            sec, perf_repaints / 5, perf_loops / 5, perf_idle_loops / 5,
            perf_key_events, perf_mouse_events, perf_commits / 5,
            (uint32_t)(perf_repaint_tsc /
                       (perf_repaints ? perf_repaints : 1) / 1000));
    perf_repaints = 0;
    perf_repaint_tsc = 0;
    perf_loops = 0;
    perf_idle_loops = 0;
    perf_key_events = 0;
    perf_mouse_events = 0;
    perf_commits = 0;
    perf_sec_mark = sec;
}

static void client_post(struct WL_CLIENT *c, int type, int32_t a, int32_t b,
                        int32_t cc) {
    if (!c || !c->used)
        return;
    lock_acquire(&c->lock);
    int next = (c->qhead + 1) % WL_CLIENT_QUEUE;
    if (next != c->qtail) {
        c->queue[c->qhead].type = type;
        c->queue[c->qhead].a = a;
        c->queue[c->qhead].b = b;
        c->queue[c->qhead].c = cc;
        c->qhead = next;
        sema_up(&c->sema);
    }
    lock_release(&c->lock);
}

void (*log_hook)(const char *s) = 0;
void comp_log(const char *s) {
    if (log_hook)
        log_hook(s);
}

void comp_lock_acquire(void) {
    lock_acquire(&comp_lock);
}

void comp_lock_release(void) {
    lock_release(&comp_lock);
}

void comp_damage_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    struct GFX_RECT r = {x, y, w, h}, scr = {0, 0, scrnx, scrny}, v;
    if (!gfx_rect_intersect(r, scr, &v))
        return;
    lock_acquire(&comp_lock);
    if (damage_full) {
        lock_release(&comp_lock);
        return;
    }

    if (damage_n >= MAX_DAMAGE) {
        damage_full = 1;
    } else {
        damage[damage_n].r = v;
        damage[damage_n].content_only = 0;
        damage_n++;
    }
    lock_release(&comp_lock);
}

void comp_damage_content(struct WL_SURFACE *s) {
    if (!s || !s->used || s->w <= 0 || s->h <= 0)
        return;
    struct GFX_RECT r = {s->x, s->y, s->w, s->h}, scr = {0, 0, scrnx, scrny}, v;
    if (!gfx_rect_intersect(r, scr, &v))
        return;
    lock_acquire(&comp_lock);
    if (damage_full) {
        lock_release(&comp_lock);
        return;
    }
    if (damage_n >= MAX_DAMAGE) {
        damage_full = 1;
    } else {
        damage[damage_n].r = v;
        damage[damage_n].content_only = 1;
        damage_n++;
    }
    lock_release(&comp_lock);
}

void comp_damage_surface(struct WL_SURFACE *s) {
    if (!s || !s->used)
        return;
    int m = GPU_SHADOW_BLUR + WIN_RADIUS + 2;
    comp_damage_rect(s->x - COMP_BORDER - m, s->y - COMP_TITLE_H - m,
                     s->w + 2 * COMP_BORDER + 2 * m,
                     s->h + COMP_TITLE_H + COMP_BORDER + 2 * m);
}

void wl_surface_set_alpha(struct WL_SURFACE *s, int alpha) {
    if (!s || !s->used)
        return;
    if (alpha < 0)
        alpha = 0;
    if (alpha > 255)
        alpha = 255;
    lock_acquire(&comp_lock);
    s->alpha = (uint8_t)alpha;
    lock_release(&comp_lock);
    comp_damage_surface(s);
}

void comp_post_key(uint8_t scancode, int pressed, uint8_t mods) {
    input_post(INPUT_DEV_KEYBOARD, scancode,
               (int32_t)pressed | ((int32_t)mods << 8));
}

void comp_post_mouse(int dx, int dy, uint8_t buttons) {
    input_post(INPUT_DEV_POINTER, (uint32_t)buttons,
               (int32_t)(dx & 0xFFFF) | ((int32_t)(dy & 0xFFFF) << 16));
}

struct WL_CLIENT *wl_display_connect(const char *name) {
    lock_acquire(&comp_lock);
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (clients[i].used)
            continue;
        struct WL_CLIENT *c = &clients[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        strncpy(c->name, name, 15);
        c->name[15] = 0;
        sema_init(&c->sema, 0);
        lock_init(&c->lock);
        lock_release(&comp_lock);
        return c;
    }
    lock_release(&comp_lock);
    return 0;
}

void wl_display_disconnect(struct WL_CLIENT *c) {
    if (!c)
        return;
    lock_acquire(&comp_lock);
    c->used = 0;
    lock_release(&comp_lock);
}

struct WL_SURFACE *wl_compositor_create_surface(struct WL_CLIENT *c,
                                                const char *title) {
    if (!c || !c->used)
        return 0;
    lock_acquire(&comp_lock);
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (surfaces[i].used)
            continue;
        struct WL_SURFACE *s = &surfaces[i];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->client = c;
        s->ws = 0;
        s->alpha = 255;
        strncpy(s->title, title, 23);
        s->title[23] = 0;
        c->surf = s;
        lock_release(&comp_lock);
        return s;
    }
    lock_release(&comp_lock);
    return 0;
}

int wl_surface_attach(struct WL_SURFACE *s, struct WL_SHM_POOL *pool, int w,
                      int h) {
    if (!s || !s->used || !pool || !pool->in_use)
        return -1;
    lock_acquire(&comp_lock);
    s->buf = pool->data;
    s->buf_w = w;
    s->buf_h = h;
    lock_release(&comp_lock);
    return 0;
}

void wl_surface_commit(struct WL_SURFACE *s) {
    if (!s || !s->used || !s->buf)
        return;
    perf_commits++;
    lock_acquire(&comp_lock);
    s->frame_pending = 1;
    s->bs_content_dirty = 1;
    int geom_ok = (s->buf_w == s->w && s->buf_h == s->h);
    lock_release(&comp_lock);
    if (geom_ok)
        comp_damage_content(s);
    else
        comp_damage_surface(s);
}

void wl_surface_destroy(struct WL_SURFACE *s) {
    if (!s || !s->used)
        return;
    lock_acquire(&comp_lock);
    if (s->client)
        s->client->surf = 0;
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (&surfaces[i] == s) {
            if (bs_slots[i].pixels == 0 && s->bs.pixels)
                bs_slots[i] = s->bs;
            break;
        }
    }
    memset(&s->bs, 0, sizeof(s->bs));
    s->bs_frame_valid = 0;
    s->bs_content_dirty = 0;
    s->used = 0;
    s->buf = 0;
    lock_release(&comp_lock);
}

int wl_display_dispatch(struct WL_CLIENT *c, struct WL_EVENT *ev) {
    if (!c || !c->used)
        return -1;
    sema_down(&c->sema);
    lock_acquire(&c->lock);
    if (c->qtail == c->qhead) {
        lock_release(&c->lock);
        ev->type = WL_EV_NONE;
        return 0;
    }
    *ev = c->queue[c->qtail];
    c->qtail = (c->qtail + 1) % WL_CLIENT_QUEUE;
    lock_release(&c->lock);
    return 0;
}

struct WL_SURFACE **comp_surfaces(int *count) {
    static struct WL_SURFACE *list[WL_MAX_SURFACES];
    int n = 0;
    lock_acquire(&comp_lock);
    for (int i = 0; i < WL_MAX_SURFACES; i++)
        if (surfaces[i].used)
            list[n++] = &surfaces[i];
    lock_release(&comp_lock);
    *count = n;
    return list;
}

int comp_pointer_x(void) {
    return cur_x;
}

int comp_pointer_y(void) {
    return cur_y;
}

uint8_t comp_pointer_buttons(void) {
    return last_buttons;
}

int comp_screen_w(void) {
    return scrnx;
}

int comp_screen_h(void) {
    return scrny;
}

void comp_send_configure(struct WL_SURFACE *s, int w, int h) {
    client_post(s->client, WL_EV_CONFIGURE, w, h, 0);
}

void comp_send_close(struct WL_SURFACE *s) {
    client_post(s->client, WL_EV_CLOSE, 0, 0, 0);
}

void comp_send_key(struct WL_SURFACE *s, int scancode, int pressed, int mods) {
    if (s->x11_owner) {
        x11_notify_key(s, scancode + 8, pressed, mods);
        return;
    }
    kprintf("send_key s=[%s] sc=%02x\n", s->title, scancode);
    client_post(s->client, WL_EV_KEY, scancode, pressed, mods);
}

void comp_request_exit(void) {
    kprintf("compositor: exit requested\n");
    session_active = 0;
}

void comp_destroy_surface_pool(struct WL_SURFACE *s, struct WL_SHM_POOL **pool) {
    if (!pool || !*pool)
        return;
    lock_acquire(&comp_lock);
    if (s) {
        s->buf = 0;
        s->buf_w = 0;
        s->buf_h = 0;
        s->bs_content_dirty = 1;
    }
    lock_release(&comp_lock);
    shm_pool_destroy(*pool);
    *pool = 0;
}

static gfx_color wallpaper_color(int y) {
    const struct GUI_THEME *t = theme();
    int grad = (scrny > 1) ? (y * 255) / (scrny - 1) : 0;
    if (grad < 0)
        grad = 0;
    if (grad > 255)
        grad = 255;
    int r = GFX_R(t->wp_top) +
            gfx_div255((GFX_R(t->wp_bot) - GFX_R(t->wp_top)) * grad);
    int g = GFX_G(t->wp_top) +
            gfx_div255((GFX_G(t->wp_bot) - GFX_G(t->wp_top)) * grad);
    int b = GFX_B(t->wp_top) +
            gfx_div255((GFX_B(t->wp_bot) - GFX_B(t->wp_top)) * grad);
    return GFX_RGB(r, g, b);
}

static struct GFX_CANVAS wp_cache;
static int wp_ready;

static void wallpaper_init(void) {
    size_t bsz = (size_t)scrnx * (size_t)scrny * 4u;
    uint8_t *bp = (uint8_t *)get_kernel_pages(
        (uint32_t)((bsz + (size_t)PAGE_SIZE - 1) / (size_t)PAGE_SIZE));
    if (bp == 0)
        return;
    wp_cache.pixels = (gfx_color *)bp;
    wp_cache.pitch = scrnx * 4;
    wp_cache.w = scrnx;
    wp_cache.h = scrny;
    wp_cache.bytes = bsz;
    for (int y = 0; y < scrny; y++) {
        gfx_color c = wallpaper_color(y);
        gfx_color *row =
            wp_cache.pixels + (size_t)y * (size_t)gfx_stride(&wp_cache);
        for (int x = 0; x < scrnx; x++)
            row[x] = c;
    }
    wp_ready = 1;
}

void comp_set_wallpaper(const uint32_t *pixels, int w, int h) {
    if (!wp_ready || !pixels || w <= 0 || h <= 0)
        return;
    struct GFX_CANVAS src;
    src.pixels = (gfx_color *)(uintptr_t)pixels;
    src.pitch = w * 4;
    src.w = w;
    src.h = h;
    src.bytes = (size_t)w * (size_t)h * 4u;
    int cx = 0, cy = 0, cw = w, ch = h;
    if ((int64_t)w * scrny > (int64_t)h * scrnx) {
        cw = (int)((int64_t)h * scrnx / scrny);
        if (cw < 1)
            cw = 1;
        cx = (w - cw) / 2;
    } else {
        ch = (int)((int64_t)w * scrny / scrnx);
        if (ch < 1)
            ch = 1;
        cy = (h - ch) / 2;
    }
    gfx_blit_scale(&wp_cache, 0, 0, scrnx, scrny, &src, cx, cy, cw, ch);
}

static void draw_wallpaper(struct GFX_RECT *r) {
    if (!wp_ready) {
        for (int y = r->y; y < r->y + r->h; y++)
            gfx_hline(dst, r->x, y, r->w, wallpaper_color(y));
        return;
    }
    int dstride = gfx_stride(dst);
    int sstride = gfx_stride(&wp_cache);
    for (int y = r->y; y < r->y + r->h; y++) {
        gfx_color *drow = dst->pixels + (size_t)y * (size_t)dstride;
        const gfx_color *srow =
            wp_cache.pixels + (size_t)y * (size_t)sstride;
        for (int x = r->x; x < r->x + r->w; x++)
            drow[x] = srow[x];
    }
}

static void frame_of(struct WL_SURFACE *s, int *fx, int *fy, int *fw, int *fh) {
    *fx = s->x - COMP_BORDER;
    *fy = s->y - COMP_TITLE_H;
    *fw = s->w + 2 * COMP_BORDER;
    *fh = s->h + COMP_TITLE_H + COMP_BORDER;
}

static int frame_covers(struct WL_SURFACE *s, struct GFX_RECT *r) {
    int fx, fy, fw, fh;
    frame_of(s, &fx, &fy, &fw, &fh);
    int m = WIN_RADIUS;
    return (r->x >= fx + m && r->y >= fy + m && r->x + r->w <= fx + fw - m &&
            r->y + r->h <= fy + fh - m);
}

static int hidden_from_above(struct WL_SURFACE **vis, int vn, int self,
                             struct GFX_RECT *r) {
    for (int k = self + 1; k < vn; k++) {
        struct WL_SURFACE *w = vis[k];
        if (w->alpha != 255)
            continue;
        if (frame_covers(w, r))
            return 1;
    }
    return 0;
}

static void push_content(struct WL_SURFACE *s, int rad) {
    struct GPU_CMD c;
    memset(&c, 0, sizeof(c));
    c.op = GPU_OP_ROUND_BLEND;
    c.rad = rad - COMP_BORDER;
    c.corners = (uint8_t)(GFX_CORNER_BL | GFX_CORNER_BR);
    c.alpha = s->alpha;
    c.x = s->x;
    c.y = s->y;
    c.w = s->w;
    c.h = s->h;
    if (s->buf && s->buf_w == s->w && s->buf_h == s->h) {
        static struct GFX_CANVAS sc;
        sc.pixels = (gfx_color *)s->buf;
        sc.pitch = s->w * 4;
        sc.w = s->w;
        sc.h = s->h;
        sc.bytes = (size_t)s->w * (size_t)s->h * 4u;
        c.src = &sc;
        c.sx = 0;
        c.sy = 0;
        gpu_push(&c);
    } else {
        c.src = 0;
        c.op = GPU_OP_ROUNDFILL;
        c.color = theme()->content;
        gpu_push(&c);
    }
}

static void bs_prepare(struct WL_SURFACE *s);
static void bs_blit(struct WL_SURFACE *s, struct GFX_RECT *clip);
static int bs_ensure_frame(struct WL_SURFACE *s);
static void bs_content_fused(struct WL_SURFACE *s, struct GFX_RECT *clip);
static void draw_window_text(struct WL_SURFACE *s, struct GFX_RECT *clip);

static void draw_window_shadow(struct WL_SURFACE *s) {
    int fx, fy, fw, fh;
    frame_of(s, &fx, &fy, &fw, &fh);
    struct GFX_CANVAS *sp =
        gpu_shadow_sprite(fw, fh, WIN_RADIUS, GPU_SHADOW_BLUR);
    if (!sp)
        return;
    struct GPU_CMD c;
    memset(&c, 0, sizeof(c));
    c.op = GPU_OP_SHADOW;
    c.alpha = s->alpha;
    c.src = sp;
    c.x = fx - GPU_SHADOW_BLUR;
    c.y = fy - GPU_SHADOW_BLUR;
    c.w = fw;
    c.h = fh;
    c.rad = GPU_SHADOW_BLUR;
    c.corners = (uint8_t)WIN_RADIUS;
    gpu_push(&c);
}

static void draw_window_body(struct WL_SURFACE *s) {
    int fx, fy, fw, fh;
    frame_of(s, &fx, &fy, &fw, &fh);
    int rad = WIN_RADIUS;
    int alpha = s->alpha;
    const struct GUI_THEME *t = theme();
    int focused = (wm_focused_surface() == s);
    gfx_color border_col = focused ? t->frame_foc : t->frame_unf;
    gfx_color title_col = focused ? t->title_foc : t->title_unf;

    struct GPU_CMD c;
    memset(&c, 0, sizeof(c));
    c.op = GPU_OP_ROUNDFILL;
    c.corners = GFX_CORNER_ALL;
    c.rad = rad;
    c.alpha = alpha;
    c.x = fx;
    c.y = fy;
    c.w = fw;
    c.h = fh;
    c.color = border_col;
    gpu_push(&c);

    c.y = fy + COMP_BORDER;
    c.h = COMP_TITLE_H - COMP_BORDER;
    c.rad = rad - COMP_BORDER;
    c.corners = (uint8_t)(GFX_CORNER_TL | GFX_CORNER_TR);
    c.color = title_col;
    gpu_push(&c);

    push_content(s, rad);

    memset(&c, 0, sizeof(c));
    c.op = GPU_OP_ROUNDFILL;
    c.rad = 5;
    c.alpha = alpha;
    c.x = fx + fw - 20;
    c.y = fy + 4;
    c.w = 15;
    c.h = 15;
    c.color = wm_hover_close(s) ? t->close : t->dim;
    gpu_push(&c);
}

static void draw_window_cached(struct WL_SURFACE *s, struct GFX_RECT *clip) {
    draw_window_shadow(s);
    gpu_batch_flush();
    bs_prepare(s);
    if (s->bs.pixels) {
        bs_blit(s, clip);
        return;
    }
    draw_window_body(s);
    gpu_batch_flush();
    draw_window_text(s, clip);
}

static void draw_window_text(struct WL_SURFACE *s, struct GFX_RECT *clip) {
    int fx, fy, fw, fh;
    frame_of(s, &fx, &fy, &fw, &fh);
    struct GFX_RECT frame = {fx, fy, fw, fh}, v;
    if (!gfx_rect_intersect(frame, *clip, &v))
        return;
    int focused = (wm_focused_surface() == s);
    const struct GUI_THEME *t = theme();
    int title_px = TITLE_FONT_PX;
    int ty = fy + (COMP_TITLE_H - font_ascent(title_px)) / 2;
    if (ty < fy)
        ty = fy;

    gfx_color fg = focused ? t->title_fg_foc : t->title_fg_unf;
    font_draw_clip(gpu_target(), fx + 8, ty, s->title, title_px, fg, &v);

    int cw = font_text_width("x", CLOSE_FONT_PX);
    font_draw_clip(gpu_target(), fx + fw - 20 + (15 - cw) / 2, fy + 4, "x",
                   CLOSE_FONT_PX, fg, &v);
}

static void draw_cursor(void) {
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            char p = cursor_bmp[row][col];
            if (p == '.')
                continue;
            int px = cur_x + col, py = cur_y + row;
            if (px < 0 || px >= scrnx || py < 0 || py >= scrny)
                continue;
            gfx_color c = (p == 'O') ? CURSOR_FILL : CURSOR_OUTLINE;
            dst->pixels[(size_t)py * (size_t)gfx_stride(dst) + (size_t)px] = c;
        }
    }
}

#define CURSOR_HW_PX 64

#define GUI_HW_CURSOR 0

static uint32_t hw_cursor_img[CURSOR_HW_PX * CURSOR_HW_PX];

static int hw_cursor;

static void hw_cursor_build(void) {
    memset(hw_cursor_img, 0, sizeof(hw_cursor_img));
    for (int row = 0; row < CURSOR_H && row < CURSOR_HW_PX; row++) {
        for (int col = 0; col < CURSOR_W && col < CURSOR_HW_PX; col++) {
            char p = cursor_bmp[row][col];
            if (p == '.')
                continue;
            hw_cursor_img[row * CURSOR_HW_PX + col] =
                (p == 'O') ? CURSOR_FILL : CURSOR_OUTLINE;
        }
    }
}

static int hw_cursor_init(void) {
    if (!GUI_HW_CURSOR)
        return -1;
    struct GUI_DISPLAY_OPS *d = display_get();
    if (d == 0 || d->cursor_set == 0 || d->cursor_move == 0)
        return -1;
    hw_cursor_build();
    if (d->cursor_set(CURSOR_HW_PX, CURSOR_HW_PX, hw_cursor_img, 0, 0) != 0)
        return -1;
    d->cursor_move(cur_x, cur_y);
    kprintf("gui: hw cursor active\n");
    return 0;
}

static gfx_color bs_blend(gfx_color d, gfx_color src, int ga) {
    int sa = GFX_A(src);
    if (sa == 0)
        return d;
    sa = gfx_div255(sa * ga + 127);
    if (sa <= 0)
        return d;
    if (sa >= 255)
        return src | 0xFF000000u;
    int dr = GFX_R(d), dg = GFX_G(d), db = GFX_B(d);
    int sr = GFX_R(src), sg = GFX_G(src), sb = GFX_B(src);
    return 0xFF000000u |
           ((uint32_t)(dr + gfx_div255((sr - dr) * sa + 127)) << 16) |
           ((uint32_t)(dg + gfx_div255((sg - dg) * sa + 127)) << 8) |
           (uint32_t)(db + gfx_div255((sb - db) * sa + 127));
}

static void bs_fill_round(struct GFX_CANVAS *cv, int x, int y, int w, int h,
                          int rad, int corners, gfx_color rgb, int over) {
    if (rad < 0)
        rad = 0;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    int top = (corners & (GFX_CORNER_TL | GFX_CORNER_TR)) ? rad : 0;
    int bot = (corners & (GFX_CORNER_BL | GFX_CORNER_BR)) ? rad : 0;
    size_t mapped = gfx_canvas_mapped_bytes(cv, &(int){0});
    for (int ly = 0; ly < h; ly++) {
        int py = y + ly;
        if (py < 0 || py >= cv->h || x < 0)
            continue;
        size_t off = (size_t)py * (size_t)cv->pitch + (size_t)x * 4u;
        if (off + (size_t)w * 4u > mapped)
            continue;
        gfx_color *row = gfx_px_at(cv, off);
        int full = (ly >= top && ly < h - bot);
        for (int lx = 0; lx < w; lx++) {
            int cov = full ? 255
                           : gfx_round_coverage(lx, ly, w, h, rad, corners);
            if (cov <= 0)
                continue;
            if (over)
                row[lx] = gfx_over(row[lx], rgb | 0xFF000000u, cov);
            else
                row[lx] = (rgb & 0x00FFFFFFu) | ((uint32_t)cov << 24);
        }
    }
}

static gfx_color bs_mix(gfx_color base, gfx_color top, int w) {
    if (w <= 0)
        return base & 0x00FFFFFFu;
    if (w >= 255)
        return top & 0x00FFFFFFu;
    int br = GFX_R(base), bg = GFX_G(base), bb = GFX_B(base);
    int tr = GFX_R(top), tg = GFX_G(top), tb = GFX_B(top);
    return ((uint32_t)(br + gfx_div255((tr - br) * w + 127)) << 16) |
           ((uint32_t)(bg + gfx_div255((tg - bg) * w + 127)) << 8) |
           (uint32_t)(bb + gfx_div255((tb - bb) * w + 127));
}

static gfx_color bs_content_px(gfx_color base_rgb, gfx_color p, int cov) {
    if (cov <= 0)
        return 0;
    int eff = gfx_div255(GFX_A(p) * cov + 127);
    return bs_mix(base_rgb, p, eff) | ((uint32_t)cov << 24);
}

static void bs_render_frame(struct WL_SURFACE *s) {
    struct GFX_CANVAS *cv = &s->bs;
    int fw = cv->w, fh = cv->h;
    const struct GUI_THEME *t = theme();
    int focused = (wm_focused_surface() == s);
    gfx_color border_col = focused ? t->frame_foc : t->frame_unf;
    gfx_color title_col = focused ? t->title_foc : t->title_unf;
    gfx_color fg = focused ? t->title_fg_foc : t->title_fg_unf;
    gfx_color close_col = wm_hover_close(s) ? t->close : t->dim;

    bs_fill_round(cv, 0, 0, fw, fh, WIN_RADIUS, GFX_CORNER_ALL, border_col, 0);
    bs_fill_round(cv, 0, COMP_BORDER, fw, COMP_TITLE_H - COMP_BORDER,
                  WIN_RADIUS - COMP_BORDER,
                  (int)(GFX_CORNER_TL | GFX_CORNER_TR), title_col, 1);
    bs_fill_round(cv, fw - 20, 4, 15, 15, 5, GFX_CORNER_ALL, close_col, 1);

    struct GFX_RECT whole = {0, 0, fw, fh};
    int ty = (COMP_TITLE_H - font_ascent(TITLE_FONT_PX)) / 2;
    if (ty < 0)
        ty = 0;
    font_draw_clip(cv, 8, ty, s->title, TITLE_FONT_PX, fg, &whole);
    int cw = font_text_width("x", CLOSE_FONT_PX);
    font_draw_clip(cv, fw - 20 + (15 - cw) / 2, 4, "x", CLOSE_FONT_PX, fg,
                   &whole);
}

static void bs_render_content(struct WL_SURFACE *s) {
    struct GFX_CANVAS *cv = &s->bs;
    int w = s->w, h = s->h;
    int cx = COMP_BORDER, cy = COMP_TITLE_H;
    int rad = WIN_RADIUS - COMP_BORDER;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    int corners = GFX_CORNER_BL | GFX_CORNER_BR;
    size_t mapped = gfx_canvas_mapped_bytes(cv, &(int){0});
    int with_buf = (s->buf && s->buf_w == w && s->buf_h == h);
    const gfx_color *src = (const gfx_color *)(const void *)s->buf;
    gfx_color fill = theme()->content | 0xFF000000u;
    const struct GUI_THEME *t = theme();
    gfx_color base_rgb =
        (wm_focused_surface() == s ? t->frame_foc : t->frame_unf) &
        0x00FFFFFFu;
    for (int ly = 0; ly < h; ly++) {
        int py = cy + ly;
        if (py < 0 || py >= cv->h)
            continue;
        size_t doff = (size_t)py * (size_t)cv->pitch + (size_t)cx * 4u;
        if (doff + (size_t)w * 4u > mapped)
            break;
        gfx_color *dp = gfx_px_at(cv, doff);
        const gfx_color *sp = with_buf ? src + (size_t)ly * (size_t)w : 0;
        int in_band = (ly >= h - rad);
        for (int lx = 0; lx < w; lx++) {
            int cov = 255;
            if (in_band)
                cov = gfx_round_coverage(lx, ly, w, h, rad, corners);
            if (cov <= 0)
                continue;
            dp[lx] = bs_content_px(base_rgb, with_buf ? sp[lx] : fill, cov);
        }
    }
    s->bs_content_dirty = 0;
}

#define BS_RESERVE_PAGES 1024

static uint32_t bs_free_pages(void) {
    const struct MM_BITMAP *b = &kernel_vaddr.vaddr_bitmap;
    uint32_t total = b->btmp_bytes_len * 8;
    uint32_t used = 0;
    if (b->bits == 0)
        return 0;
    for (uint32_t i = 0; i < b->btmp_bytes_len; i++) {
        uint8_t byte = b->bits[i];
        while (byte) {
            used += (uint32_t)(byte & 1u);
            byte >>= 1;
        }
    }
    return (total > used) ? total - used : 0;
}

static int bs_ensure_frame(struct WL_SURFACE *s) {
    int idx = -1;
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (&surfaces[i] == s) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -1;
    struct GFX_CANVAS *slot = &bs_slots[idx];
    int fw = s->w + 2 * COMP_BORDER;
    int fh = s->h + COMP_TITLE_H + COMP_BORDER;
    if (fw <= 0 || fh <= 0)
        return -1;
    if (s->bs.pixels == 0) {
        if (slot->pixels && slot->w == fw && slot->h == fh) {
            s->bs = *slot;
        } else {
            size_t bsz = (size_t)fw * (size_t)fh * 4u;
            uint32_t pages = (uint32_t)((bsz + PAGE_SIZE - 1) / PAGE_SIZE);
            size_t need = (size_t)pages * PAGE_SIZE;
            if (bs_free_pages() < pages + BS_RESERVE_PAGES) {
                static int once;
                if (!once) {
                    once = 1;
                    kprintf("gui: bs cache skipped (pool low)\n");
                }
                return -1;
            }
            uint8_t *bp = (uint8_t *)get_kernel_pages(pages);
            if (bp == 0)
                return -1;
            memset(bp, 0, need);
            s->bs.pixels = (gfx_color *)bp;
            s->bs.pitch = fw * 4;
            s->bs.w = fw;
            s->bs.h = fh;
            s->bs.bytes = bsz;
        }
        s->bs_frame_valid = 0;
        s->bs_content_dirty = 1;
    } else if (s->bs.w != fw || s->bs.h != fh) {
        if (slot->pixels == 0)
            *slot = s->bs;
        memset(&s->bs, 0, sizeof(s->bs));
        return bs_ensure_frame(s);
    }
    if (s->bs_frame_valid == 0) {
        bs_render_frame(s);
        s->bs_frame_valid = 1;
        s->bs_content_dirty = 1;
    }
    return 0;
}

static void bs_prepare(struct WL_SURFACE *s) {
    if (bs_ensure_frame(s) != 0)
        return;
    if (s->bs_content_dirty)
        bs_render_content(s);
}

static void bs_content_fused(struct WL_SURFACE *s, struct GFX_RECT *clip) {
    struct GFX_CANVAS *cv = &s->bs;
    if (cv->pixels == 0 || dst == 0)
        return;
    int w = s->w, h = s->h;
    int rad = WIN_RADIUS - COMP_BORDER;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    int corners = GFX_CORNER_BL | GFX_CORNER_BR;
    int ga = s->alpha;
    int with_buf = (s->buf && s->buf_w == w && s->buf_h == h);
    const gfx_color *src = (const gfx_color *)(const void *)s->buf;
    const struct GUI_THEME *t = theme();
    gfx_color fill = t->content | 0xFF000000u;
    gfx_color base_rgb =
        (wm_focused_surface() == s ? t->frame_foc : t->frame_unf) &
        0x00FFFFFFu;
    size_t cmapped = gfx_canvas_mapped_bytes(cv, &(int){0});
    size_t dmapped = gfx_canvas_mapped_bytes(dst, &(int){0});
    for (int ly = 0; ly < h; ly++) {
        size_t coff = (size_t)(COMP_TITLE_H + ly) * (size_t)cv->pitch +
                      (size_t)COMP_BORDER * 4u;
        if (coff + (size_t)w * 4u > cmapped)
            break;
        gfx_color *crow = gfx_px_at(cv, coff);
        const gfx_color *sp = with_buf ? src + (size_t)ly * (size_t)w : 0;
        int py = s->y + ly;
        gfx_color *drow = 0;
        if (py >= clip->y && py < clip->y + clip->h && py >= 0 && py < scrny) {
            size_t doff = (size_t)py * (size_t)dst->pitch + (size_t)s->x * 4u;
            if (doff + (size_t)w * 4u <= dmapped)
                drow = gfx_px_at(dst, doff);
        }
        int in_band = (ly >= h - rad);
        for (int lx = 0; lx < w; lx++) {
            int cov = 255;
            if (in_band)
                cov = gfx_round_coverage(lx, ly, w, h, rad, corners);
            if (cov <= 0)
                continue;
            gfx_color c = bs_content_px(base_rgb, with_buf ? sp[lx] : fill,
                                        cov);
            crow[lx] = c;
            if (drow == 0)
                continue;
            int px = s->x + lx;
            if (px < clip->x || px >= clip->x + clip->w || px < 0 || px >= scrnx)
                continue;
            drow[lx] = bs_blend(drow[lx], c, ga);
        }
    }
    s->bs_content_dirty = 0;
}

static void bs_blit(struct WL_SURFACE *s, struct GFX_RECT *clip) {
    struct GFX_CANVAS *b = &s->bs;
    if (b->pixels == 0 || dst == 0)
        return;
    int fx = s->x - COMP_BORDER;
    int fy = s->y - COMP_TITLE_H;
    struct GFX_RECT f = {fx, fy, b->w, b->h}, v;
    if (!gfx_rect_intersect(f, *clip, &v))
        return;
    int ga = s->alpha;
    if (ga <= 0)
        return;
    int rad = WIN_RADIUS;
    if (rad * 2 > b->h)
        rad = b->h / 2;
    size_t mapped = gfx_canvas_mapped_bytes(dst, &(int){0});
    size_t smapped = gfx_canvas_mapped_bytes(b, &(int){0});
    int lx = v.x + v.w - 1, ly_last = v.y + v.h - 1;
    size_t dend = (size_t)ly_last * (size_t)dst->pitch + (size_t)lx * 4u + 4u;
    size_t send = (size_t)(ly_last - fy) * (size_t)b->pitch +
                  (size_t)(lx - fx) * 4u + 4u;
    if (dend > mapped || send > smapped)
        return;
    for (int py = v.y; py < v.y + v.h; py++) {
        gfx_color *dp = gfx_px_at(
            dst, (size_t)py * (size_t)dst->pitch + (size_t)v.x * 4u);
        const gfx_color *sp = gfx_px_at_c(
            b, (size_t)(py - fy) * (size_t)b->pitch + (size_t)(v.x - fx) * 4u);
        if (ga >= 255) {
            int ly = py - fy;
            if (ly >= rad && ly < b->h - rad) {
                memcpy(dp, sp, (size_t)v.w * sizeof(gfx_color));
                continue;
            }
            for (int i = 0; i < v.w; i++) {
                gfx_color c = sp[i];
                int sa = GFX_A(c);
                if (sa == 0)
                    continue;
                dp[i] = (sa >= 255) ? c : bs_blend(dp[i], c, 255);
            }
        } else {
            for (int i = 0; i < v.w; i++) {
                gfx_color c = sp[i];
                if (GFX_A(c) == 0)
                    continue;
                dp[i] = bs_blend(dp[i], c, ga);
            }
        }
    }
}

void comp_surface_invalidate(struct WL_SURFACE *s) {
    if (!s || !s->used)
        return;
    s->bs_frame_valid = 0;
}

void comp_invalidate_all(void) {
    for (int i = 0; i < WL_MAX_SURFACES; i++)
        if (surfaces[i].used)
            surfaces[i].bs_frame_valid = 0;
}

static int rect_covered_by_window(struct GFX_RECT *r, struct WL_SURFACE **vis,
                                  int vn) {
    for (int j = 0; j < vn; j++) {
        struct WL_SURFACE *s = vis[j];
        if (s->alpha != 255)
            continue;
        if (frame_covers(s, r))
            return 1;
    }
    return 0;
}

static void repaint(void);
static void comp_present(struct GFX_RECT *rects, int n);

static int frame_hits(struct WL_SURFACE *s, struct GFX_RECT *r) {
    struct GFX_RECT f, v;
    int fx, fy, fw, fh;
    frame_of(s, &fx, &fy, &fw, &fh);
    f.x = fx;
    f.y = fy;
    f.w = fw;
    f.h = fh;
    return gfx_rect_intersect(f, *r, &v);
}

static int repaint_content_only(struct GUI_DAMAGE *d, struct WL_SURFACE **vis,
                                int vn) {
    struct GFX_RECT *r = &d->r;
    for (int j = vn - 1; j >= 0; j--) {
        struct WL_SURFACE *s = vis[j];
        if (r->x < s->x || r->y < s->y || r->x + r->w > s->x + s->w ||
            r->y + r->h > s->y + s->h)
            continue;
        if (s->alpha != 255)
            return 0;
        gpu_batch_clip(r);
        if (bs_ensure_frame(s) == 0)
            bs_content_fused(s, r);
        else
            push_content(s, WIN_RADIUS);
        gpu_batch_flush();
        for (int k = j + 1; k < vn; k++) {
            struct WL_SURFACE *t = vis[k];
            if (!frame_hits(t, r))
                continue;
            if (hidden_from_above(vis, vn, k, r))
                continue;
            draw_window_cached(t, r);
        }
        wm_draw_overlay(dst, r);
        return 1;
    }
    return 0;
}

static void repaint(void) {
    struct GUI_DAMAGE rects[MAX_DAMAGE + 1];
    struct GFX_RECT flip[MAX_DAMAGE + 1];
    int n = 0;
    lock_acquire(&comp_lock);
    if (damage_full) {
        rects[0].r.x = 0;
        rects[0].r.y = 0;
        rects[0].r.w = scrnx;
        rects[0].r.h = scrny;
        rects[0].content_only = 0;
        n = 1;
    } else {
        for (int i = 0; i < damage_n; i++)
            rects[i] = damage[i];
        n = damage_n;
    }
    damage_n = 0;
    damage_full = 0;

    struct WL_SURFACE *vis[WL_MAX_SURFACES];
    int vn = wm_collect_visible(vis, WL_MAX_SURFACES);

    gpu_batch_begin();
    for (int i = 0; i < n; i++) {
        struct GFX_RECT *r = &rects[i].r;
        flip[i] = *r;
        if (rects[i].content_only && repaint_content_only(&rects[i], vis, vn))
            continue;
        if (!rect_covered_by_window(r, vis, vn))
            draw_wallpaper(r);
        for (int j = 0; j < vn; j++) {
            struct WL_SURFACE *s = vis[j];
            if (hidden_from_above(vis, vn, j, r))
                continue;
            gpu_batch_clip(r);
            draw_window_cached(s, r);
        }
        wm_draw_bar(dst, r);
        wm_draw_overlay(dst, r);
    }
    gpu_batch_clip(0);
    lock_release(&comp_lock);
    comp_present(flip, n);

    for (int i = 0; i < vn; i++) {
        struct WL_SURFACE *s = vis[i];
        if (s->frame_pending) {
            s->frame_pending = 0;
            client_post(s->client, WL_EV_FRAME, 0, 0, 0);
        }
    }
}

static void comp_present(struct GFX_RECT *rects, int n) {
    struct GUI_DISPLAY_OPS *d = display_get();
    if (d == 0)
        return;
    if (!hw_cursor)
        draw_cursor();
    d->flip(rects, n);
}

void comp_init(void) {
    display_init();
    struct GUI_DISPLAY_OPS *d = display_get();
    d->init();

    scrnx = io_get_scrnx();
    scrny = io_get_scrny();
    dst = d->surface(DISP_BACK);
    gpu_set_target(dst);

    struct GFX_CANVAS *fc = d->surface(DISP_FRONT);
    (void)fc;

    input_init();
    wallpaper_init();
    lock_init(&comp_lock);
    shm_init();
    memset(clients, 0, sizeof(clients));
    memset(surfaces, 0, sizeof(surfaces));
    damage_n = 0;
    damage_full = 1;
    cur_x = scrnx / 2;
    cur_y = scrny / 2;
    last_buttons = 0;
    session_active = 1;
    hw_cursor = (hw_cursor_init() == 0);
}

static void drain_input(void) {
    struct GUI_INPUT_EVENT ev;
    int cur_dirty = 0;
    int motion_pending = 0;
    lock_acquire(&comp_lock);
    while (input_get(&ev)) {
        if (ev.dev == INPUT_DEV_KEYBOARD) {
            perf_key_events++;
            wm_handle_key((uint8_t)ev.code, (int)(ev.value & 0xFF),
                          (uint8_t)(ev.value >> 8));
        } else if (ev.dev == INPUT_DEV_POINTER) {
            perf_mouse_events++;
            int dx = (int16_t)(ev.value & 0xFFFF);
            int dy = (int16_t)((uint32_t)ev.value >> 16);
            int nx = cur_x + dx;
            int ny = cur_y + dy;
            if (nx < 0)
                nx = 0;
            if (nx >= scrnx)
                nx = scrnx - 1;
            if (ny < 0)
                ny = 0;
            if (ny >= scrny)
                ny = scrny - 1;
            if (nx != cur_x || ny != cur_y) {
                if (!hw_cursor) {
                    comp_damage_rect(cur_x, cur_y, CURSOR_W, CURSOR_H);
                    comp_damage_rect(nx, ny, CURSOR_W, CURSOR_H);
                }
                cur_x = nx;
                cur_y = ny;
                motion_pending = 1;
                cur_dirty = 1;
            }
            uint8_t btn = (uint8_t)ev.code;
            uint8_t edge = btn ^ last_buttons;
            if (edge) {
                if (motion_pending) {
                    motion_pending = 0;
                    wm_handle_motion(cur_x, cur_y);
                    wm_handle_hover(cur_x, cur_y);
                    struct WL_SURFACE *mh = wm_surface_at(cur_x, cur_y);
                    if (mh && mh->x11_owner)
                        x11_notify_motion(mh, cur_x - mh->x, cur_y - mh->y);
                }
                wm_handle_button(cur_x, cur_y, btn, edge);
                struct WL_SURFACE *hit = wm_surface_at(cur_x, cur_y);
                if (hit && hit->x11_owner) {
                    int b = (edge & 1) ? 1 : 3;
                    x11_notify_button(hit, cur_x - hit->x, cur_y - hit->y, b,
                                      (btn & 1) ? 1 : 0);
                }
                last_buttons = btn;
            }
        }
    }
    if (motion_pending) {
        wm_handle_motion(cur_x, cur_y);
        wm_handle_hover(cur_x, cur_y);
        struct WL_SURFACE *mh = wm_surface_at(cur_x, cur_y);
        if (mh && mh->x11_owner)
            x11_notify_motion(mh, cur_x - mh->x, cur_y - mh->y);
    }
    if (hw_cursor && cur_dirty) {
        struct GUI_DISPLAY_OPS *d = display_get();
        if (d && d->cursor_move)
            d->cursor_move(cur_x, cur_y);
    }
    lock_release(&comp_lock);
}

static void frame_clock(void) {
    lock_acquire(&comp_lock);
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        struct WL_SURFACE *s = &surfaces[i];
        if (!s->used || !s->buf || s->frame_pending)
            continue;
        client_post(s->client, WL_EV_FRAME, 0, 0, 0);
    }
    lock_release(&comp_lock);
}

void comp_run(void) {
    struct GUI_DISPLAY_OPS *d = display_get();
    while (session_active) {
        perf_report();
        uint64_t t0 = perf_tsc();
        drain_input();
        wm_anim_step();
        if (wm_bar_check_dirty()) {
            comp_damage_rect(0, scrny - COMP_BAR_H, scrnx, COMP_BAR_H);
        }
        lock_acquire(&comp_lock);
        int pending = (damage_full || damage_n > 0);
        lock_release(&comp_lock);
        if (pending) {
            uint64_t t1 = perf_tsc();
            repaint();
            perf_repaints++;
            perf_repaint_tsc += perf_tsc() - t1;
            perf_loops++;
            (void)t0;
            continue;
        }
        perf_idle_loops++;
        perf_loops++;
        frame_clock();
        if (d && d->wait_vblank)
            d->wait_vblank();
        else
            mtime_sleep(20);
    }
}
