#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "drivers/char/ioqueue.h"

extern struct TTY_IOQUEUE keyboard_ioq;

#define KBD_MOD_SHIFT 1
#define KBD_MOD_CTRL 2
#define KBD_MOD_ALT 4

#define KBD_SC_1 0x02
#define KBD_SC_Q 0x10
#define KBD_SC_E 0x12
#define KBD_SC_T 0x14
#define KBD_SC_U 0x16
#define KBD_SC_A 0x1E
#define KBD_SC_S 0x1F
#define KBD_SC_L 0x26
#define KBD_SC_M 0x32
#define KBD_SC_ALT 0x38
#define KBD_SC_SPACE 0x39
#define KBD_SC_ENTER 0x1C
#define KBD_SC_TAB 0x0F
#define KBD_SC_BACKSPACE 0x0E
#define KBD_SC_UP 0x48
#define KBD_SC_DOWN 0x50
#define KBD_SC_LEFT 0x4B
#define KBD_SC_RIGHT 0x4D

typedef void (*kbd_gui_hook_t)(uint8_t scancode, int pressed, uint8_t mods);

void keyboard_init(void);
void keyboard_flush_pending(void);
void keyboard_handler(void);
void keyboard_set_gui_hook(kbd_gui_hook_t hook);
char keyboard_translate(uint8_t scancode, int shift);

#endif
