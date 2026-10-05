#ifndef GUI_UDI_H
#define GUI_UDI_H

#include "kernel/gui/gfx.h"
#include <stdint.h>

struct GUI_UDI_BUFFER {
    uint64_t handle;
    void *vmem;
    uint32_t w;
    uint32_t h;
    uint32_t bpp;
    uint32_t size;
};

struct GUI_UDI_OPS {
    const char *name;
    int (*probe)(void);
    int (*init)(uint32_t *w, uint32_t *h, uint32_t bpp);
    int (*alloc_buffer)(uint32_t w, uint32_t h, uint32_t bpp,
                        struct GUI_UDI_BUFFER *out);
    void (*free_buffer)(uint64_t handle);
    int (*commit)(uint64_t handle, struct GFX_RECT *rects, int n);
    void (*wait_vblank)(void);
    int (*cursor_set)(int w, int h, const void *argb, int hot_x, int hot_y);
    int (*cursor_move)(int x, int y);
};

int udi_init(uint32_t *w, uint32_t *h, uint32_t bpp);
struct GUI_UDI_OPS *udi_active(void);
void udi_register(struct GUI_UDI_OPS *ops);
int udi_cursor_set(int w, int h, const void *argb, int hot_x, int hot_y);
int udi_cursor_move(int x, int y);

#endif
