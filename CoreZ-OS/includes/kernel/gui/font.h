#ifndef GUI_FONT_H
#define GUI_FONT_H
#include "kernel/gui/gfx.h"
int font_init(const void *ttf_data, int ttf_len);
int font_ready(void);
int font_ascent(int px);
int font_line_height(int px);
int font_text_width(const char *utf8, int px);
int font_draw(struct GFX_CANVAS *c, int x, int y, const char *utf8, int px,
              gfx_color fg);
int font_draw_clip(struct GFX_CANVAS *c, int x, int y, const char *utf8, int px,
                   gfx_color fg, const struct GFX_RECT *clip);
int font_utf8_next(const char **sp);
#endif
