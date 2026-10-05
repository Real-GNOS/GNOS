#include "drivers/char/mouse.h"
#include "drivers/driver_ops.h"

#include "kernel/asm_func.h"
#include "drivers/char/keyboard.h"

#define KBD_DATA 0x60
#define KBD_STATUS 0x64
#define KBD_CMD 0x64

static mouse_hook_t hook = 0;
static uint8_t pkt[3];
static int idx = 0;

static void ctrl_wait_write(void) {
    while (inb(KBD_STATUS) & 0x02) {
    }
}

static void ctrl_wait_read(void) {
    while (!(inb(KBD_STATUS) & 0x01)) {
    }
}

static void aux_write(uint8_t v) {
    ctrl_wait_write();
    outb(KBD_CMD, 0xD4);
    ctrl_wait_write();
    outb(KBD_DATA, v);
}

static uint8_t aux_read(void) {
    ctrl_wait_read();
    return inb(KBD_DATA);
}

void mouse_set_hook(mouse_hook_t cb) {
    hook = cb;
}

static int mouse_drv(void) {
    mouse_init();
    return 0;
}

DRIVER_REGISTER("mouse", 15, mouse_drv);

void mouse_init(void) {
    keyboard_flush_pending();

    ctrl_wait_write();
    outb(KBD_CMD, 0xA8);

    ctrl_wait_write();
    outb(KBD_CMD, 0x20);
    ctrl_wait_read();
    uint8_t status = inb(KBD_DATA);
    status |= 0x02;
    status &= (uint8_t)~0x20;
    ctrl_wait_write();
    outb(KBD_CMD, 0x60);
    ctrl_wait_write();
    outb(KBD_DATA, status);

    aux_write(0xF6);
    (void)aux_read();
    aux_write(0xF4);
    (void)aux_read();
}

void mouse_handler(void) {
    uint8_t b = inb(KBD_DATA);

    if (idx == 0 && !(b & 0x08))
        return;
    pkt[idx++] = b;
    if (idx < 3)
        return;
    idx = 0;

    int dx = (int)pkt[1] - ((int)(pkt[0] << 4) & 0x100);
    int dy = (int)pkt[2] - ((int)(pkt[0] << 3) & 0x100);
    uint8_t buttons = pkt[0] & 0x07;

    if (hook)
        hook(dx, -dy, buttons);
}
