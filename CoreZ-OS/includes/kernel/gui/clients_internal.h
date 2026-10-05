#ifndef GUI_CLIENTS_INTERNAL_H
#define GUI_CLIENTS_INTERNAL_H

#include "drivers/char/keyboard.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/sched/thread.h"
#include "kernel/gui/font.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/server.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/theme.h"
#include "kernel/gui/wm.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/dir.h"
#include "kernel/mm/pool/pool.h"
#include "lib/png/png.h"

#define UI_FONT_PX 13

extern void (*log_hook)(const char *s);

struct COMP_DEMO_CLIENT {
    struct WL_CLIENT *conn;
    struct WL_SURFACE *surf;
    struct WL_SHM_POOL *pool;
    int w, h;
    uint32_t frame_interval;
    uint32_t last_frame;
    int last_dark;
    void (*render)(struct COMP_DEMO_CLIENT *dc);
    void (*on_key)(struct COMP_DEMO_CLIENT *dc, int scancode, int mods);
};

struct CLIENT_DESC {
    const char *name;
    const char *title;
    void (*render)(struct COMP_DEMO_CLIENT *dc);
    void (*on_key)(struct COMP_DEMO_CLIENT *dc, int scancode, int mods);
    uint32_t frame_interval;
};

static inline void client_repaint(struct COMP_DEMO_CLIENT *dc) {
    dc->render(dc);
    wl_surface_commit(dc->surf);
}

void canvas_of(struct GFX_CANVAS *cv, struct COMP_DEMO_CLIENT *dc);
int buffer_resize(struct COMP_DEMO_CLIENT *dc, int w, int h);
void attach_commit(struct COMP_DEMO_CLIENT *dc);
void client_main(struct COMP_DEMO_CLIENT *dc);
int client_begin(struct COMP_DEMO_CLIENT *dc, const struct CLIENT_DESC *desc);
char sc_to_char(uint8_t sc, int shift);
void s_copy(char *dst, const char *src, int cap);
void s_join(char *dst, const char *dir, const char *name, int cap);

void clients_term_thread(void *arg);
void clients_clock_thread(void *arg);
void clients_sysmon_thread(void *arg);
void clients_plasma_thread(void *arg);
void clients_png_thread(void *arg);
void clients_files_thread(void *arg);
void clients_edit_thread(void *arg);

#endif
