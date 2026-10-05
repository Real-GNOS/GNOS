#ifndef GUI_WM_INTERNAL_H
#define GUI_WM_INTERNAL_H

#include "kernel/gui/wm.h"

#include "arch/interrupt/interrupt.h"
#include "kernel/gui/font.h"
#include "kernel/init/pit/pit.h"
#include "lib/str/str.h"
#include "drivers/char/console/io.h"

#define TASKBAR_TOP (comp_screen_h() - COMP_BAR_H)
#define BAR_FONT_PX 12
#define BAR_PILL_W 24
#define TASKBAR_H (COMP_BAR_H - 10)
#define TASKBAR_BTN_MIN 78
#define TASKBAR_BTN_MAXW 170

void wm_anim_start(struct WL_SURFACE *s, int target);

void wm_bar_reset(void);
void wm_bar_invalidate(void);
void wm_bar_hover(int x, int y);
void wm_bar_click(int x, int y);

void wm_focus_index(int idx);
void wm_switch_ws(int target);
int wm_ws_occupied(int ws);

#endif
