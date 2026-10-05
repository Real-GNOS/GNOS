#ifndef GUI_SHM_H
#define GUI_SHM_H

#include "kernel/sched/sync.h"
#include <stdint.h>

enum WL_BUF_STATE {
    WLBUF_FREE = 0,
    WLBUF_READY = 1,
    WLBUF_DIRTY = 2,
};

struct WL_SHM_POOL {
    uint8_t *data;
    uint32_t size;
    uint32_t pages;
    int in_use;
    int buf_id;
};

void shm_init(void);
struct WL_SHM_POOL *shm_pool_create(uint32_t size);
void shm_pool_destroy(struct WL_SHM_POOL *pool);

int wl_buffer_create(uint32_t size);
void *wl_buffer_map(int id);
void wl_buffer_ref(int id);
void wl_buffer_release(int id);
uint32_t wl_buffer_state(int id);
void wl_buffer_set_state(int id, uint32_t st);

#endif
