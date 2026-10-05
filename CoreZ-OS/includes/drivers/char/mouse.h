#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

typedef void (*mouse_hook_t)(int dx, int dy, uint8_t buttons);

void mouse_init(void);
void mouse_handler(void);
void mouse_set_hook(mouse_hook_t hook);

#endif
