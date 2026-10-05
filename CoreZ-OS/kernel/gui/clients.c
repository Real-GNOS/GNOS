#include "kernel/gui/clients.h"

#include "kernel/gui/clients_internal.h"

void canvas_of(struct GFX_CANVAS *cv, struct COMP_DEMO_CLIENT *dc) {
    cv->pixels = (gfx_color *)dc->pool->data;
    cv->pitch = dc->w * 4;
    cv->w = dc->w;
    cv->h = dc->h;
    cv->bytes = (size_t)dc->w * (size_t)dc->h * 4u;
}

int buffer_resize(struct COMP_DEMO_CLIENT *dc, int w, int h) {
    if (w <= 0 || h <= 0)
        return -1;
    if (dc->pool && dc->w == w && dc->h == h)
        return 0;
    if (dc->pool) {
        comp_destroy_surface_pool(dc->surf, &dc->pool);
    }
    dc->pool = shm_pool_create((uint32_t)(w * h * 4));
    if (!dc->pool)
        return -1;
    dc->w = w;
    dc->h = h;
    return 0;
}

void attach_commit(struct COMP_DEMO_CLIENT *dc) {
    if (!dc->pool)
        return;
    wl_surface_attach(dc->surf, dc->pool, dc->w, dc->h);
    wl_surface_commit(dc->surf);
}

void client_main(struct COMP_DEMO_CLIENT *dc) {
    for (;;) {
        struct WL_EVENT ev;
        if (wl_display_dispatch(dc->conn, &ev) != 0)
            break;
        switch (ev.type) {
        case WL_EV_CONFIGURE:
            if (buffer_resize(dc, (int)ev.a, (int)ev.b) == 0) {
                dc->render(dc);
                attach_commit(dc);
            }
            break;
        case WL_EV_FRAME:
            if (dc->pool && (tick - dc->last_frame >= dc->frame_interval ||
                             dc->last_dark != theme()->dark)) {
                dc->last_frame = tick;
                dc->last_dark = theme()->dark;
                client_repaint(dc);
            }
            break;
        case WL_EV_KEY:
            if (dc->on_key && ev.b)
                dc->on_key(dc, (int)ev.a, (int)ev.c);
            break;
        case WL_EV_CLOSE:
            goto out;
        default:
            break;
        }
    }
out:
    wm_unmanage(dc->surf);
    wl_surface_destroy(dc->surf);
    if (dc->pool)
        comp_destroy_surface_pool(dc->surf, &dc->pool);
    wl_display_disconnect(dc->conn);
    dc->conn = 0;
    thread_exit_current();
}

char sc_to_char(uint8_t sc, int shift) {
    static const char *lo1 = "1234567890-=";
    static const char *hi1 = "!@#$%^&*()_+";
    static const char *lo2 = "qwertyuiop[]";
    static const char *hi2 = "QWERTYUIOP{}";
    static const char *lo3 = "asdfghjkl;'";
    static const char *hi3 = "ASDFGHJKL:\"";
    static const char *lo4 = "zxcvbnm,./";
    static const char *hi4 = "ZXCVBNM<>?";
    int i;
    if (sc >= 0x02 && sc <= 0x0D) {
        i = sc - 0x02;
        return (shift ? hi1 : lo1)[i];
    }
    if (sc >= 0x10 && sc <= 0x1B) {
        i = sc - 0x10;
        return (shift ? hi2 : lo2)[i];
    }
    if (sc >= 0x1E && sc <= 0x28) {
        i = sc - 0x1E;
        return (shift ? hi3 : lo3)[i];
    }
    if (sc == 0x29)
        return shift ? '~' : '`';
    if (sc == 0x2B)
        return shift ? '|' : '\\';
    if (sc >= 0x2C && sc <= 0x35) {
        i = sc - 0x2C;
        return (shift ? hi4 : lo4)[i];
    }
    if (sc == 0x39)
        return ' ';
    return 0;
}

void s_copy(char *dst, const char *src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

void s_join(char *dst, const char *dir, const char *name, int cap) {
    s_copy(dst, dir, cap);
    int l = strlen(dst);
    if (l > 0 && dst[l - 1] != '/' && l < cap - 1)
        dst[l++] = '/';
    dst[l] = 0;
    s_copy(dst, strcat(dst, name), cap);
}

int client_begin(struct COMP_DEMO_CLIENT *dc,
                 const struct CLIENT_DESC *desc) {
    dc->conn = wl_display_connect(desc->name);
    if (!dc->conn)
        return -1;
    dc->surf = wl_compositor_create_surface(dc->conn, desc->title);
    if (!dc->surf) {
        wl_display_disconnect(dc->conn);
        dc->conn = 0;
        return -1;
    }
    dc->render = desc->render;
    dc->on_key = desc->on_key;
    dc->frame_interval = desc->frame_interval;
    wm_manage(dc->surf);
    return 0;
}

typedef void (*client_thread_fn)(void *);

static client_thread_fn types[] = {
    clients_clock_thread, clients_sysmon_thread, clients_png_thread,
    clients_term_thread,  clients_plasma_thread, clients_files_thread,
    clients_edit_thread};
static const char *type_names[] = {"gc_clock", "gc_sysmon", "gc_pngview",
                                   "gc_term", "gc_plasma", "gc_files",
                                   "gc_edit"};
#define CLIENT_TYPES ((int)(sizeof(types) / sizeof(types[0])))
static int next_type = 0;

void clients_spawn_next(void) {
    int type_idx = next_type % CLIENT_TYPES;
    next_type++;
    kernel_thread((char *)type_names[type_idx], 6, types[type_idx], 0, 0xF);
}

void clients_spawn_initial(void) {
    next_type = 3;
    clients_spawn_next();
    next_type = 3;
    clients_spawn_next();
}

void clients_broadcast_close(void) {
    int n = 0;
    struct WL_SURFACE **list = comp_surfaces(&n);
    for (int i = 0; i < n; i++)
        comp_send_close(list[i]);
}
