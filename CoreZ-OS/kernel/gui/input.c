#include "kernel/gui/input.h"

#define INPUT_RING 64

static volatile struct GUI_INPUT_EVENT ring[INPUT_RING];
static volatile uint32_t head;
static volatile uint32_t tail;

void input_init(void) {
    head = 0;
    tail = 0;
}

int input_post(uint32_t dev, uint32_t code, int32_t value) {
    if (dev == INPUT_DEV_NONE)
        return -1;
    uint32_t next = (head + 1) % INPUT_RING;
    if (next == tail)
        return -1;
    ring[head].dev = dev;
    ring[head].code = code;
    ring[head].value = value;
    head = next;
    return 0;
}

int input_get(struct GUI_INPUT_EVENT *ev) {
    if (tail == head)
        return 0;
    ev->dev = ring[tail].dev;
    ev->code = ring[tail].code;
    ev->value = ring[tail].value;
    tail = (tail + 1) % INPUT_RING;
    return 1;
}
