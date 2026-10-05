#ifndef LIB_TTF_TTF_H
#define LIB_TTF_TTF_H
#include <stdint.h>

#define TTF_BOX_W 8
#define TTF_BOX_H 16
#define TTF_PX 13

int ttf_console_init(const void *data, uint32_t len);
int ttf_ready(void);
const uint8_t *ttf_mask(uint8_t ch);
#endif