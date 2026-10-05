#ifndef GUI_INPUT_H
#define GUI_INPUT_H

#include <stdint.h>

enum GUI_INPUT_DEV_TYPE {
    INPUT_DEV_NONE = 0,
    INPUT_DEV_KEYBOARD = 1,
    INPUT_DEV_POINTER = 2,
};

struct GUI_INPUT_EVENT {
    uint32_t dev;
    uint32_t code;
    int32_t value;
};

void input_init(void);
int input_post(uint32_t dev, uint32_t code, int32_t value);
int input_get(struct GUI_INPUT_EVENT *ev);

#endif
