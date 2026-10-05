#ifndef GUI_DISPLAY_H
#define GUI_DISPLAY_H

#include "kernel/gui/gfx.h"
#include <stdint.h>

enum GUI_SURFACE { DISP_FRONT = 0, DISP_BACK = 1 };

struct GUI_DISPLAY_OPS {
    const char *name;
    int (*init)(void);
    struct GFX_CANVAS *(*surface)(int which);
    int (*flip)(struct GFX_RECT *rects, int n);
    void (*wait_vblank)(void);
    int (*set_mode)(uint32_t w, uint32_t h, uint32_t bpp);
    int (*cursor_set)(int w, int h, const void *argb, int hot_x, int hot_y);
    int (*cursor_move)(int x, int y);
};

void display_init(void);
void display_register(struct GUI_DISPLAY_OPS *ops);
struct GUI_DISPLAY_OPS *display_get(void);

#endif
