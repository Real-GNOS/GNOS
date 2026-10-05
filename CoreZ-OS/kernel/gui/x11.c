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
struct X11_CONN conns[X11_MAX_CONNS];
uint32_t xid_next = X11_ROOT_WINDOW + 1;
uint32_t gid_next = 0x06000200;
uint32_t pid_next = 0x06000400;
uint32_t atom_next = 100;

void x11_gateway_init(void) {
    memset(conns, 0, sizeof(conns));
    xid_next = X11_ROOT_WINDOW + 1;
    gid_next = 0x06000200;
    pid_next = 0x06000400;
    atom_next = 100;
    comp_log("x11gw: gateway ready (proto 11.0)");
}

struct X11_CONN *x11_conn_open(void) {
    for (int i = 0; i < X11_MAX_CONNS; i++) {
        if (conns[i].used)
            continue;
        struct X11_CONN *c = &conns[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->seq = 0;
        c->client_id = (uint32_t)(i + 1);
        lock_init(&c->lock);
        return c;
    }
    return 0;
}

void x11_conn_close(struct X11_CONN *c) {
    if (!c || !c->used)
        return;
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (!c->win[i].used)
            continue;
        if (c->win[i].s) {
            c->win[i].s->x11_owner = 0;
            wm_unmanage(c->win[i].s);
            wl_surface_destroy(c->win[i].s);
        }
        if (c->win[i].pool)
            shm_pool_destroy(c->win[i].pool);
    }
    for (int i = 0; i < X11_MAX_PIXMAPS; i++)
        if (c->pix[i].used && c->pix[i].data)
            free_kernel_page((uint32_t)c->pix[i].data);
    memset(c, 0, sizeof(*c));
}







int out_bytes(struct X11_CONN *c, const void *p, uint32_t n) {
    lock_acquire(&c->lock);
    int ok = 1;
    if (c->out_head + n > X11_OUT_BUF) {
        uint32_t live = c->out_head - c->out_tail;
        if (live + n > X11_OUT_BUF) {
            ok = 0;
        } else {
            memmove(c->out, c->out + c->out_tail, live);
            c->out_head = live;
            c->out_tail = 0;
        }
    }
    if (ok) {
        memcpy(c->out + c->out_head, p, n);
        c->out_head += n;
    }
    lock_release(&c->lock);
    if (!ok)
        c->dead = 1;
    return ok ? 0 : -1;
}

uint32_t x11_conn_drain(struct X11_CONN *c, uint8_t *buf, uint32_t len) {
    uint32_t n;
    lock_acquire(&c->lock);
    n = c->out_head - c->out_tail;
    if (n > len)
        n = len;
    if (n > 0) {
        memcpy(buf, c->out + c->out_tail, n);
        c->out_tail += n;
        if (c->out_tail == c->out_head)
            c->out_head = c->out_tail = 0;
    }
    lock_release(&c->lock);
    return n;
}

void post_error(struct X11_CONN *c, uint8_t code, uint8_t major,
                       uint16_t minor, uint32_t res) {
    uint8_t ev[32];
    memset(ev, 0, sizeof(ev));
    ev[1] = code;
    wr16(ev + 2, (uint16_t)c->seq);
    wr32(ev + 4, res);
    wr16(ev + 8, minor);
    ev[10] = major;
    out_bytes(c, ev, 32);
}

void reply_init(struct X11_CONN *c, uint8_t major, uint32_t extra,
                       uint8_t *head) {
    memset(head, 0, 32);
    head[0] = 1;
    head[1] = major;
    wr16(head + 2, (uint16_t)c->seq);
    wr32(head + 4, extra / 4);
}

void reply_finish(struct X11_CONN *c, const uint8_t *head,
                         const uint8_t *extra, uint32_t n) {
    out_bytes(c, head, 32);
    if (n)
        out_bytes(c, extra, n);
}

void out_event(struct X11_CONN *c, const uint8_t *ev) {
    out_bytes(c, ev, 32);
}

















































static void dispatch(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    uint8_t op = p[0];
    uint8_t minor = p[1];
    switch (op) {
    case X11_REQ_CreateWindow:
        h_create_window(c, p, len);
        break;
    case X11_REQ_ChangeWindowAttributes:
        h_change_attrs(c, p, len);
        break;
    case X11_REQ_GetWindowAttributes:
        h_get_window_attrs(c, p, len);
        break;
    case X11_REQ_DestroyWindow:
        h_destroy_window(c, p, len);
        break;
    case X11_REQ_MapWindow:
        h_map_window(c, p, len);
        break;
    case X11_REQ_UnmapWindow:
        h_unmap_window(c, p, len);
        break;
    case X11_REQ_ConfigureWindow:
        h_configure_window(c, p, len);
        break;
    case X11_REQ_GetGeometry:
        h_get_geometry(c, p, len);
        break;
    case X11_REQ_QueryTree:
        h_query_tree(c, p, len);
        break;
    case X11_REQ_InternAtom:
        h_intern_atom(c, p, len);
        break;
    case X11_REQ_ChangeProperty:
        h_change_property(c, p, len);
        break;
    case X11_REQ_GetProperty:
        h_get_property(c, p, len);
        break;
    case X11_REQ_DeleteProperty:
        h_delete_property(c, p, len);
        break;
    case X11_REQ_QueryPointer:
        h_query_pointer(c, p, len);
        break;
    case X11_REQ_TranslateCoordinates:
        h_translate_coords(c, p, len);
        break;
    case X11_REQ_GetInputFocus:
        h_get_input_focus(c, p, len);
        break;
    case X11_REQ_SetInputFocus:
        h_set_input_focus(c, p, len);
        break;
    case X11_REQ_OpenFont:
        h_open_font(c, p, len);
        break;
    case X11_REQ_QueryTextExtents:
        h_query_text_extents(c, p, len);
        break;
    case X11_REQ_CreatePixmap:
        h_create_pixmap(c, p, len);
        break;
    case X11_REQ_FreePixmap:
        h_free_pixmap(c, p, len);
        break;
    case X11_REQ_CreateGC:
        h_create_gc(c, p, len);
        break;
    case X11_REQ_ChangeGC:
        h_change_gc(c, p, len);
        break;
    case X11_REQ_FreeGC:
        h_free_gc(c, p, len);
        break;
    case X11_REQ_ClearArea:
        h_clear_area(c, p, len);
        break;
    case X11_REQ_CopyArea:
        h_copy_area(c, p, len);
        break;
    case X11_REQ_PolyPoint:
        h_poly_point(c, p, len);
        break;
    case X11_REQ_PolyLine:
        h_poly_line(c, p, len);
        break;
    case X11_REQ_PolySegment:
        h_poly_segment(c, p, len);
        break;
    case X11_REQ_PolyRectangle:
        h_poly_rectangle(c, p, len);
        break;
    case X11_REQ_PolyArc:
        h_poly_arc(c, p, len);
        break;
    case X11_REQ_PolyFillArc:
        h_poly_fill_arc(c, p, len);
        break;
    case X11_REQ_FillPoly:
        h_fill_poly(c, p, len);
        break;
    case X11_REQ_PolyFillRectangle:
        h_poly_fill_rect(c, p, len);
        break;
    case X11_REQ_PutImage:
        h_put_image(c, p, len);
        break;
    case X11_REQ_GetImage:
        h_get_image(c, p, len);
        break;
    case X11_REQ_AllocColor:
        h_alloc_color(c, p, len);
        break;
    case X11_REQ_AllocNamedColor:
        h_alloc_named_color(c, p, len);
        break;
    case X11_REQ_QueryColors:
        h_query_colors(c, p, len);
        break;
    case X11_REQ_LookupColor:
        h_lookup_color(c, p, len);
        break;
    case X11_REQ_QueryExtension:
        h_query_extension(c, p, len);
        break;
    case X11_REQ_ListExtensions:
        h_list_extensions(c, p, len);
        break;
    case X11_REQ_KillClient:
        c->dead = 1;
        break;
    case X11_REQ_SetCloseDownMode:
    case X11_REQ_SetAccessControl:
    case X11_REQ_Bell:
    case X11_REQ_SetScreenSaver:
    case X11_REQ_InstallColormap:
    case X11_REQ_FreeColormap:
    case X11_REQ_CreateColormap:
    case X11_REQ_SetDashes:
    case X11_REQ_SetClipRectangles:
    case X11_REQ_CopyGC:
    case X11_REQ_CloseFont:
    case X11_REQ_ImageText8:
    case X11_REQ_NoOperation:
        break;
    default:
        post_error(c, X11_ERR_Request, op, minor, 0);
        break;
    }
}

static int send_setup_reply(struct X11_CONN *c) {
    const char *vendor = "NCS X11 Gateway";
    uint32_t vlen = (uint32_t)strlen(vendor);
    uint32_t vpad = (vlen + 3) & ~3u;
    uint32_t add = vpad + 8 + 40 + 8 + 24;
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    uint32_t o = 8;
    buf[0] = 1;
    wr16(buf + 2, X11_PROTO_MAJOR);
    wr16(buf + 4, X11_PROTO_MINOR);
    wr16(buf + 6, (uint16_t)(add / 4));
    uint8_t body[24];
    memset(body, 0, sizeof(body));
    wr32(body + 0, 0x00010000u);
    wr32(body + 4, 0x06000000u);
    wr32(body + 8, 0x000FFFFFu);
    wr32(body + 12, 0);
    wr16(body + 16, (uint16_t)vlen);
    wr16(body + 18, 256);
    body[20] = 1;
    body[21] = 1;
    body[22] = 0;
    body[23] = 0;
    uint8_t pad8[8];
    memset(pad8, 0, sizeof(pad8));
    pad8[0] = 32;
    pad8[1] = 32;
    pad8[2] = 8;
    pad8[3] = 255;
    (void)o;
    uint8_t out[256];
    memset(out, 0, sizeof(out));
    memcpy(out, buf, 8);
    memcpy(out + 8, body, 24);
    uint32_t w = 32;
    memcpy(out + w, pad8, 8);
    w += 8;
    memcpy(out + w, vendor, vlen);
    w += vpad;
    out[w++] = 24;
    out[w++] = 32;
    out[w++] = 32;
    out[w++] = 0;
    w += 4;
    wr32(out + w, X11_ROOT_WINDOW);
    w += 4;
    wr32(out + w, X11_DEFAULT_COLORMAP);
    w += 4;
    wr32(out + w, 0xFFFFFFu);
    w += 4;
    wr32(out + w, 0x000000u);
    w += 4;
    wr32(out + w, 0);
    w += 4;
    wr16(out + w, (uint16_t)comp_screen_w());
    w += 2;
    wr16(out + w, (uint16_t)comp_screen_h());
    w += 2;
    wr16(out + w, 340);
    w += 2;
    wr16(out + w, 255);
    w += 2;
    wr16(out + w, 1);
    w += 2;
    wr16(out + w, 1);
    w += 2;
    wr32(out + w, X11_ROOT_VISUAL);
    w += 4;
    out[w++] = 0;
    out[w++] = 0;
    out[w++] = 24;
    out[w++] = 1;
    out[w++] = 24;
    out[w++] = 0;
    wr16(out + w, 1);
    w += 2;
    w += 4;
    wr32(out + w, X11_ROOT_VISUAL);
    w += 4;
    out[w++] = 4;
    out[w++] = 8;
    wr16(out + w, 256);
    w += 2;
    wr32(out + w, 0x00FF0000u);
    w += 4;
    wr32(out + w, 0x0000FF00u);
    w += 4;
    wr32(out + w, 0x000000FFu);
    w += 4;
    w += 4;
    return out_bytes(c, out, w);
}

static int handshake(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return 0;
    uint8_t order = p[0];
    uint16_t nlen = rd16(p + 6);
    uint16_t dlen = rd16(p + 8);
    uint32_t need = 12 + ((nlen + 3) & ~3u) + ((dlen + 3) & ~3u);
    if (len < need)
        return 0;
    if (send_setup_reply(c) != 0) {
        c->dead = 1;
        return -1;
    }
    kprintf("x11gw: client connected (%s, proto %u.%u)\n",
            order == 0x6C ? "LSBFirst" : "MSBFirst", rd16(p + 2), rd16(p + 4));
    c->seq = 0;
    return (int)need;
}

int x11_conn_feed(struct X11_CONN *c, const uint8_t *data, uint32_t len) {
    if (!c || !c->used || !data)
        return -1;
    uint32_t off = 0;
    if (c->seq == 0 && c->req_len == 0 && len >= 1 &&
        (data[0] == 0x6C || data[0] == 0x42)) {
        int used = handshake(c, data, len);
        if (used < 0)
            return 0;
        if (used == 0)
            return (int)len;
        off = (uint32_t)used;
    }
    while (off < len) {
        if (c->req_need == 0) {
            uint32_t avail = len - off;
            if (c->req_len < 4 && avail >= 4 - c->req_len) {
                memcpy(c->req + c->req_len, data + off, 4 - c->req_len);
                off += 4 - c->req_len;
                c->req_len = 4;
            } else if (c->req_len < 4) {
                memcpy(c->req + c->req_len, data + off, avail);
                c->req_len += avail;
                return (int)off;
            }
            uint16_t units = rd16(c->req + 2);
            if (units == 0)
                return -1;
            c->req_need = (uint32_t)units * 4u;
            if (c->req_need > X11_REQ_BUF)
                return -1;
        }
        uint32_t want = c->req_need - c->req_len;
        uint32_t avail = len - off;
        uint32_t take = want < avail ? want : avail;
        if (c->req_len + take > X11_REQ_BUF)
            return -1;
        memcpy(c->req + c->req_len, data + off, take);
        c->req_len += take;
        off += take;
        if (c->req_len == c->req_need) {
            c->seq = (uint32_t)(c->seq + 1) & 0xFFFFu;
            dispatch(c, c->req, c->req_need);
            c->req_len = 0;
            c->req_need = 0;
        }
    }
    return (int)off;
}



