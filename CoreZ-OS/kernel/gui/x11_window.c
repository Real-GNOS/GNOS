#include "kernel/gui/x11.h"

#include <stddef.h>
#include <stdint.h>

#include "drivers/char/console/io.h"
#include "kernel/gui/gfx.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/wm.h"
#include "lib/str/str.h"

#include "kernel/gui/x11_internal.h"

int win_idx(struct X11_CONN *c, uint32_t xid) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++)
        if (c->win[i].used && c->win[i].xid == xid)
            return i;
    return -1;
}

uint32_t atom_intern(struct X11_CONN *c, const char *name, int len,
                            int only_if_exists, int *found) {
    for (int i = 0; i < c->atom_n; i++) {
        if ((int)strlen(c->atoms[i].name) == len &&
            memcmp(c->atoms[i].name, name, (size_t)len) == 0) {
            if (found)
                *found = 1;
            return c->atoms[i].atom;
        }
    }
    if (only_if_exists) {
        if (found)
            *found = 0;
        return 0;
    }
    if (c->atom_n >= X11_MAX_ATOMS) {
        if (found)
            *found = 0;
        return 0;
    }
    int n = len < 23 ? len : 23;
    memcpy(c->atoms[c->atom_n].name, name, (size_t)n);
    c->atoms[c->atom_n].name[n] = 0;
    c->atoms[c->atom_n].atom = atom_next++;
    if (found)
        *found = 1;
    return c->atoms[c->atom_n++].atom;
}

int win_realize(struct X11_CONN *c, int i) {
    struct X11_WINDOW *w = &c->win[i];
    if (w->s)
        return 0;
    if (w->w == 0 || w->h == 0)
        return -1;
    uint32_t bytes = (uint32_t)w->w * (uint32_t)w->h * 4u;
    struct WL_SHM_POOL *pool = shm_pool_create(bytes);
    if (!pool)
        return -1;
    char title[24];
    title[0] = 0;
    strcat(title, "x11:");
    char num[12];
    u32_to_dec(w->xid & 0xFFFFFFu, num);
    strcat(title, num);
    struct WL_CLIENT *cl = wl_display_connect("x11gw");
    if (!cl) {
        shm_pool_destroy(pool);
        return -1;
    }
    struct WL_SURFACE *s = wl_compositor_create_surface(cl, title);
    if (!s) {
        shm_pool_destroy(pool);
        wl_display_disconnect(cl);
        return -1;
    }
    w->s = s;
    w->pool = pool;
    s->x11_owner = c;
    s->x11_xid = w->xid;
    struct GFX_CANVAS cv;
    memset(&cv, 0, sizeof(cv));
    cv.pixels = (gfx_color *)pool->data;
    cv.pitch = (int)(w->w * 4u);
    cv.w = (int)w->w;
    cv.h = (int)w->h;
    cv.bytes = bytes;
    gfx_fill(&cv, 0, 0, cv.w, cv.h, pix_to_color(w->bg_pixel));
    int bp = pix_idx(c, w->bg_pixmap);
    if (bp >= 0 && c->pix[bp].data) {
        struct GFX_CANVAS src;
        memset(&src, 0, sizeof(src));
        src.pixels = (gfx_color *)c->pix[bp].data;
        src.pitch = (int)(c->pix[bp].w * 4u);
        src.w = (int)c->pix[bp].w;
        src.h = (int)c->pix[bp].h;
        src.bytes = c->pix[bp].size;
        gfx_blit(&cv, 0, 0, &src, 0, 0, src.w, src.h);
    }
    wl_surface_attach(s, pool, (int)w->w, (int)w->h);
    wl_surface_commit(s);
    wm_manage(s);

    s->x = w->x;
    s->y = w->y;
    s->w = (int)w->w;
    s->h = (int)w->h;
    comp_damage_surface(s);
    return 0;
}
void win_teardown(struct X11_CONN *c, int i) {
    struct X11_WINDOW *w = &c->win[i];
    if (w->s) {
        w->s->x11_owner = 0;
        wm_unmanage(w->s);
        wl_surface_destroy(w->s);
        w->s = 0;
    }
    if (w->pool) {
        shm_pool_destroy(w->pool);
        w->pool = 0;
    }
    w->used = 0;
    w->mapped = 0;
}
void post_event_to(struct X11_CONN *c, struct X11_WINDOW *w,
                          const uint8_t *ev, uint32_t mask_bit) {
    if (!w || !w->used || !(w->event_mask & mask_bit))
        return;
    out_event(c, ev);
}
void h_create_window(struct X11_CONN *c, const uint8_t *p,
                            uint32_t len) {
    if (len < 32)
        return post_error(c, X11_ERR_Length, X11_REQ_CreateWindow, 0, 0);
    uint32_t wid = rd32(p + 4);
    if (win_idx(c, wid) >= 0)
        return post_error(c, X11_ERR_Value, X11_REQ_CreateWindow, 0, wid);
    int slot = -1;
    for (int i = 0; i < X11_MAX_WINDOWS; i++)
        if (!c->win[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreateWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[slot];
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->xid = wid;
    w->depth = p[1];
    w->x = (int16_t)rd16(p + 12);
    w->y = (int16_t)rd16(p + 14);
    w->w = rd16(p + 16);
    w->h = rd16(p + 18);
    w->border = rd16(p + 20);
    w->kind = p[22];
    w->bg_pixel = 0xFFFFFFu;
    w->colormap = X11_DEFAULT_COLORMAP;
    uint32_t mask = rd32(p + 28);
    const uint8_t *v = p + 32;
    uint32_t remain = len - 32;
    for (int bit = 0; bit < 15 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        switch (1u << bit) {
        case X11_CWBackPixmap:
            w->bg_pixmap = val;
            break;
        case X11_CWBackPixel:
            w->bg_pixel = val;
            break;
        case X11_CWOverrideRedirect:
            w->override_redirect = (val != 0);
            break;
        case X11_CWEventMask:
            w->event_mask = val;
            break;
        case X11_CWColormap:
            w->colormap = val;
            break;
        default:
            break;
        }
    }
    if (win_realize(c, slot) != 0) {
        w->used = 0;
        post_error(c, X11_ERR_Alloc, X11_REQ_CreateWindow, 0, wid);
    }
}
void h_change_attrs(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_ChangeWindowAttributes, 0,
                          0);
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ChangeWindowAttributes, 0,
                          wid);
    struct X11_WINDOW *w = &c->win[i];
    uint32_t mask = rd32(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int bg_changed = 0;
    for (int bit = 0; bit < 15 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        switch (1u << bit) {
        case X11_CWBackPixel:
            w->bg_pixel = val;
            bg_changed = 1;
            break;
        case X11_CWBackPixmap:
            w->bg_pixmap = val;
            bg_changed = 1;
            break;
        case X11_CWEventMask:
            w->event_mask = val;
            break;
        case X11_CWOverrideRedirect:
            w->override_redirect = (val != 0);
            break;
        case X11_CWColormap:
            w->colormap = val;
            break;
        default:
            break;
        }
    }
    if (bg_changed && w->s && w->pool) {
        struct GFX_CANVAS cv;
        memset(&cv, 0, sizeof(cv));
        cv.pixels = (gfx_color *)w->pool->data;
        cv.pitch = (int)(w->w * 4u);
        cv.w = (int)w->w;
        cv.h = (int)w->h;
        cv.bytes = (size_t)w->w * (size_t)w->h * 4u;
        gfx_fill(&cv, 0, 0, cv.w, cv.h, pix_to_color(w->bg_pixel));
        wl_surface_commit(w->s);
    }
}
void h_get_window_attrs(struct X11_CONN *c, const uint8_t *p,
                               uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_GetWindowAttributes, 0,
                          wid);
    struct X11_WINDOW *w = &c->win[i];
    uint8_t head[32];
    uint8_t extra[12];
    memset(extra, 0, sizeof(extra));
    reply_init(c, X11_REQ_GetWindowAttributes, sizeof(extra), head);
    head[1] = 0;
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_VISUAL);
    wr16(b + 4, w->kind);
    b[6] = 0;
    b[7] = 0;
    wr32(b + 8, 0);
    wr32(b + 12, 0);
    b[16] = 0;
    b[17] = 1;
    b[18] = (uint8_t)(w->mapped ? 2 : 0);
    b[19] = (uint8_t)(w->override_redirect ? 1 : 0);
    wr32(b + 20, w->colormap);
    wr32(extra + 0, w->event_mask);
    wr32(extra + 4, w->event_mask);
    wr16(extra + 8, 0);
    reply_finish(c, head, extra, sizeof(extra));
}
void h_destroy_window(struct X11_CONN *c, const uint8_t *p,
                             uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_DestroyWindow, 0, wid);
    win_teardown(c, i);
}
void h_map_window(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_MapWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[i];
    if (win_realize(c, i) != 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_MapWindow, 0, wid);
    w->mapped = 1;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = X11_EV_MAP_NOTIFY;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 4, wid);
    wr32(ev + 8, X11_ROOT_WINDOW);
    ev[12] = (uint8_t)(w->override_redirect ? 1 : 0);
    post_event_to(c, w, ev, 0x00020000u );

    memset(ev, 0, sizeof(ev));
    ev[0] = X11_EV_EXPOSE;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 4, wid);
    wr16(ev + 12, w->w);
    wr16(ev + 14, w->h);
    post_event_to(c, w, ev, 0x00008000u );
}
void h_unmap_window(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_UnmapWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[i];
    if (w->s) {
        w->s->x11_owner = 0;
        wm_unmanage(w->s);
        wl_surface_destroy(w->s);
        w->s = 0;
    }
    if (w->pool) {
        shm_pool_destroy(w->pool);
        w->pool = 0;
    }
    w->mapped = 0;
}
void h_configure_window(struct X11_CONN *c, const uint8_t *p,
                               uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_ConfigureWindow, 0, 0);
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ConfigureWindow, 0, wid);
    struct X11_WINDOW *w = &c->win[i];
    uint16_t mask = rd16(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int32_t nx = w->x, ny = w->y;
    uint32_t nw = w->w, nh = w->h;
    int resize = 0, move = 0;
    for (int bit = 0; bit < 15 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        switch (bit) {
        case 0:
            nx = (int32_t)val;
            move = 1;
            break;
        case 1:
            ny = (int32_t)val;
            move = 1;
            break;
        case 2:
            nw = val & 0xFFFFu;
            resize = 1;
            break;
        case 3:
            nh = val & 0xFFFFu;
            resize = 1;
            break;
        default:
            break;
        }
    }
    if (!w->s)
        return;
    w->x = clo16(nx);
    w->y = clo16(ny);
    if (resize && nw > 0 && nh > 0 && (nw != w->w || nh != w->h)) {
        struct WL_SHM_POOL *np = shm_pool_create(nw * nh * 4u);
        if (!np)
            return post_error(c, X11_ERR_Alloc, X11_REQ_ConfigureWindow, 0,
                              wid);
        if (w->pool)
            shm_pool_destroy(w->pool);
        w->pool = np;
        w->w = nw;
        w->h = nh;
        struct GFX_CANVAS cv;
        memset(&cv, 0, sizeof(cv));
        cv.pixels = (gfx_color *)np->data;
        cv.pitch = (int)(nw * 4u);
        cv.w = (int)nw;
        cv.h = (int)nh;
        cv.bytes = (size_t)nw * (size_t)nh * 4u;
        gfx_fill(&cv, 0, 0, cv.w, cv.h, pix_to_color(w->bg_pixel));
        wl_surface_attach(w->s, np, (int)nw, (int)nh);
    }
    if (move || resize) {
        comp_damage_surface(w->s);
        w->s->x = w->x;
        w->s->y = w->y;
        w->s->w = (int)w->w;
        w->s->h = (int)w->h;
        comp_send_configure(w->s, (int)w->w, (int)w->h);
        comp_damage_surface(w->s);
    }
}
void h_get_geometry(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    uint8_t head[32];
    reply_init(c, X11_REQ_GetGeometry, 0, head);
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_WINDOW);
    int wi = win_idx(c, did);
    head[1] = 24;
    if (wi >= 0) {
        struct X11_WINDOW *w = &c->win[wi];
        wr16(b + 4, (uint16_t)w->x);
        wr16(b + 6, (uint16_t)w->y);
        wr16(b + 8, w->w);
        wr16(b + 10, w->h);
        wr16(b + 12, w->border);
        head[1] = w->depth;
    } else {
        wr16(b + 8, (uint16_t)comp_screen_w());
        wr16(b + 10, (uint16_t)comp_screen_h());
    }
    reply_finish(c, head, 0, 0);
}
void h_query_tree(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    if (wid != X11_ROOT_WINDOW && win_idx(c, wid) < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_QueryTree, 0, wid);
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryTree, 0, head);
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_WINDOW);
    wr32(b + 4, 0);
    wr16(b + 8, 0);
    reply_finish(c, head, 0, 0);
}
void h_intern_atom(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 8)
        return post_error(c, X11_ERR_Length, X11_REQ_InternAtom, 0, 0);
    uint16_t nl = rd16(p + 4);
    int only = p[1];
    const char *name = (const char *)(p + 8);
    if ((uint32_t)nl > len - 8)
        return post_error(c, X11_ERR_Length, X11_REQ_InternAtom, 0, 0);
    int found = 0;
    uint32_t atom = atom_intern(c, name, nl, only, &found);
    uint8_t head[32];
    reply_init(c, X11_REQ_InternAtom, 0, head);
    wr32(head + 8, atom);
    reply_finish(c, head, 0, 0);
}
void h_change_property(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    if (len < 24)
        return post_error(c, X11_ERR_Length, X11_REQ_ChangeProperty, 0, 0);
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ChangeProperty, 0, wid);
    uint32_t prop = rd32(p + 8);
    uint32_t type = rd32(p + 12);
    uint8_t format = p[16];
    uint32_t n = rd32(p + 20);
    uint32_t nbytes = n * (format / 8);
    if (nbytes > 64)
        nbytes = 64;
    if (nbytes > len - 24)
        nbytes = len - 24;
    struct X11_WINDOW *w = &c->win[i];
    int slot = -1;
    for (int k = 0; k < X11_MAX_PROPS; k++)
        if (w->props[k].used && w->props[k].atom == prop) {
            slot = k;
            break;
        }
    if (slot < 0)
        for (int k = 0; k < X11_MAX_PROPS; k++)
            if (!w->props[k].used) {
                slot = k;
                break;
            }
    if (slot < 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_ChangeProperty, 0, prop);
    memset(&w->props[slot], 0, sizeof(w->props[slot]));
    w->props[slot].used = 1;
    w->props[slot].atom = prop;
    w->props[slot].type = type;
    w->props[slot].format = format;
    w->props[slot].nbytes = nbytes;
    if (nbytes)
        memcpy(w->props[slot].data, p + 24, nbytes);
}
void h_get_property(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_GetProperty, 0, wid);
    uint32_t prop = rd32(p + 8);
    struct X11_WINDOW *w = &c->win[i];
    uint8_t head[32];
    uint8_t extra[64];
    memset(extra, 0, sizeof(extra));
    uint32_t nbytes = 0;
    reply_init(c, X11_REQ_GetProperty, 0, head);
    for (int k = 0; k < X11_MAX_PROPS; k++) {
        if (!w->props[k].used || w->props[k].atom != prop)
            continue;
        head[1] = w->props[k].format;
        uint8_t *b = head + 8;
        wr32(b + 0, w->props[k].type);
        wr32(b + 4, 0);
        wr32(b + 8, w->props[k].nbytes / (w->props[k].format / 8));
        memcpy(extra, w->props[k].data, w->props[k].nbytes);
        nbytes = ((w->props[k].nbytes + 3) / 4) * 4;
        break;
    }
    wr32(head + 4, nbytes / 4);
    reply_finish(c, head, extra, nbytes);
}
void h_delete_property(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int i = win_idx(c, wid);
    if (i < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_DeleteProperty, 0, wid);
    uint32_t prop = rd32(p + 8);
    for (int k = 0; k < X11_MAX_PROPS; k++)
        if (c->win[i].props[k].used && c->win[i].props[k].atom == prop)
            c->win[i].props[k].used = 0;
}
void h_query_pointer(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)p;
    (void)len;
    int px = comp_pointer_x(), py = comp_pointer_y();
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryPointer, 0, head);
    head[1] = 1;
    uint8_t *b = head + 8;
    wr32(b + 0, X11_ROOT_WINDOW);
    wr32(b + 4, 0);
    wr16(b + 8, (uint16_t)px);
    wr16(b + 10, (uint16_t)py);
    wr16(b + 12, (uint16_t)px);
    wr16(b + 14, (uint16_t)py);
    wr16(b + 16, (uint16_t)comp_pointer_buttons());
    reply_finish(c, head, 0, 0);
}
void h_translate_coords(struct X11_CONN *c, const uint8_t *p,
                               uint32_t len) {
    (void)len;
    int wi = win_idx(c, rd32(p + 4));
    if (wi < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_TranslateCoordinates, 0,
                          rd32(p + 4));
    struct X11_WINDOW *w = &c->win[wi];
    int16_t sx = (int16_t)rd16(p + 12), sy = (int16_t)rd16(p + 14);
    uint8_t head[32];
    reply_init(c, X11_REQ_TranslateCoordinates, 0, head);
    head[1] = 1;
    uint8_t *b = head + 8;
    wr32(b + 0, 0);
    wr16(b + 4, (uint16_t)(sx + w->x));
    wr16(b + 6, (uint16_t)(sy + w->y));
    reply_finish(c, head, 0, 0);
}
void h_get_input_focus(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_GetInputFocus, 0, head);
    head[1] = 0;
    wr32(head + 8, c->focus);
    reply_finish(c, head, 0, 0);
}
void h_set_input_focus(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    if (wid != 0 && wid != 1 && win_idx(c, wid) < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_SetInputFocus, 0, wid);
    c->focus = wid;
}

void h_query_extension(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryExtension, 0, head);
    head[1] = 0;
    head[8] = 0;
    reply_finish(c, head, 0, 0);
}
void h_list_extensions(struct X11_CONN *c, const uint8_t *p,
                              uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_ListExtensions, 0, head);
    head[1] = 0;
    reply_finish(c, head, 0, 0);
}

int owner_of(struct WL_SURFACE *s, struct X11_CONN **out,
                    struct X11_WINDOW **wout) {
    if (!s || !s->x11_owner)
        return -1;
    struct X11_CONN *c = (struct X11_CONN *)s->x11_owner;
    int i = win_idx(c, s->x11_xid);
    if (i < 0)
        return -1;
    *out = c;
    *wout = &c->win[i];
    return 0;
}
void x11_notify_key(struct WL_SURFACE *s, int keycode, int pressed, int mods) {
    struct X11_CONN *c;
    struct X11_WINDOW *w;
    if (owner_of(s, &c, &w) != 0)
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)(pressed ? X11_EV_KEY_PRESS : X11_EV_KEY_RELEASE);
    ev[1] = (uint8_t)(keycode & 0xFF);
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 8, X11_ROOT_WINDOW);
    wr32(ev + 12, w->xid);
    wr16(ev + 20, (uint16_t)comp_pointer_x());
    wr16(ev + 22, (uint16_t)comp_pointer_y());
    wr16(ev + 24, (uint16_t)comp_pointer_x());
    wr16(ev + 26, (uint16_t)comp_pointer_y());
    wr16(ev + 28, (uint16_t)((mods & MOD_SHIFT) ? 0x0001 : 0));
    post_event_to(c, w, ev, pressed ? 0x00000001u : 0x00000002u);
}
void x11_notify_button(struct WL_SURFACE *s, int x, int y, int button,
                       int pressed) {
    struct X11_CONN *c;
    struct X11_WINDOW *w;
    if (owner_of(s, &c, &w) != 0)
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = (uint8_t)(pressed ? X11_EV_BUTTON_PRESS : X11_EV_BUTTON_RELEASE);
    ev[1] = (uint8_t)(button & 0xFF);
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 8, X11_ROOT_WINDOW);
    wr32(ev + 12, w->xid);
    wr16(ev + 20, (uint16_t)x);
    wr16(ev + 22, (uint16_t)y);
    wr16(ev + 24, (uint16_t)comp_pointer_x());
    wr16(ev + 26, (uint16_t)comp_pointer_y());
    post_event_to(c, w, ev, pressed ? 0x00000004u : 0x00000008u);
}
void x11_notify_motion(struct WL_SURFACE *s, int x, int y) {
    struct X11_CONN *c;
    struct X11_WINDOW *w;
    if (owner_of(s, &c, &w) != 0)
        return;
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[0] = X11_EV_MOTION_NOTIFY;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 8, X11_ROOT_WINDOW);
    wr32(ev + 12, w->xid);
    wr16(ev + 20, (uint16_t)x);
    wr16(ev + 22, (uint16_t)y);
    wr16(ev + 24, (uint16_t)comp_pointer_x());
    wr16(ev + 26, (uint16_t)comp_pointer_y());
    post_event_to(c, w, ev, 0x00000040u);
}
