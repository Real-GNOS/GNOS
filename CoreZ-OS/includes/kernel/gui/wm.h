#ifndef GUI_WM_H
#define GUI_WM_H

#include "kernel/gui/server.h"
#include <stdint.h>

void wm_init_state(void);

void wm_handle_key(uint8_t scancode, int pressed, uint8_t mods);
void wm_handle_motion(int x, int y);
void wm_handle_button(int x, int y, uint8_t buttons, uint8_t edge);

void wm_manage(struct WL_SURFACE *s);
void wm_unmanage(struct WL_SURFACE *s);

int wm_collect_visible(struct WL_SURFACE **out, int max);
struct WL_SURFACE *wm_focused_surface(void);
struct WL_SURFACE *wm_surface_at(int x, int y);
int wm_hover_close(struct WL_SURFACE *s);
void wm_handle_hover(int x, int y);
int wm_anim_step(void);
void wm_draw_overlay(struct GFX_CANVAS *c, struct GFX_RECT *clip);
int wm_current_ws(void);
void wm_draw_bar(struct GFX_CANVAS *c, struct GFX_RECT *clip);
int wm_bar_check_dirty(void);

#endif
