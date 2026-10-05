#ifndef GUI_GPU_H
#define GUI_GPU_H

#include "kernel/gui/gfx.h"
#include <stdint.h>

enum GPU_OP {
    GPU_OP_FILL = 1,
    GPU_OP_COPY,
    GPU_OP_BLEND,
    GPU_OP_ROUND_BLEND,
    GPU_OP_ROUNDFILL,
    GPU_OP_SHADOW,

};

struct GPU_CMD {
    uint8_t op;
    uint8_t corners;
    uint8_t pad[2];
    int32_t rad;
    int32_t alpha;
    struct GFX_CANVAS *src;
    int32_t sx, sy;
    int32_t x, y, w, h;
    gfx_color color;
};

void gpu_set_target(struct GFX_CANVAS *c);
struct GFX_CANVAS *gpu_target(void);
void gpu_batch_begin(void);
void gpu_batch_clip(const struct GFX_RECT *r);
void gpu_push(const struct GPU_CMD *c);
void gpu_batch_flush(void);
struct GFX_CANVAS *gpu_shadow_sprite(int fw, int fh, int rad, int blur);

#define GPU_SHADOW_BLUR 14

#endif
