#ifndef GUI_THEME_H
#define GUI_THEME_H

#include "kernel/gui/gfx.h"

struct GUI_THEME {
    const char *name;
    int dark;
    gfx_color bar;
    gfx_color bar_line;
    gfx_color accent;
    gfx_color frame_foc;
    gfx_color frame_unf;
    gfx_color title_foc;
    gfx_color title_unf;
    gfx_color title_fg_foc;
    gfx_color title_fg_unf;
    gfx_color content;
    gfx_color text;
    gfx_color muted;
    gfx_color dim;
    gfx_color close;
    gfx_color wp_top;
    gfx_color wp_bot;
};

const struct GUI_THEME *theme(void);
const struct GUI_THEME *theme_toggle(void);

#endif
