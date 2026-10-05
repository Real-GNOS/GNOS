#include "drivers/char/keyboard.h"
#include "drivers/driver_ops.h"
#include "drivers/char/tty.h"
#include "drivers/char/console/io.h"

#include "kernel/asm_func.h"
#include "kernel/sched/thread.h"

#define KEYBOARD_DATA 0x60
#define KEYBOARD_STATUS 0x64

void keyboard_flush_pending(void) {
    while (inb(KEYBOARD_STATUS) & 0x01) {
        (void)inb(KEYBOARD_DATA);
    }
}

static int keyboard_drv(void) {
    keyboard_init();
    return 0;
}

DRIVER_REGISTER("keyboard", 10, keyboard_drv);

void keyboard_init(void) {
    ioq_init(&keyboard_ioq);
    keyboard_flush_pending();
}

#define SC_SHIFT_L_DOWN 0x2A
#define SC_SHIFT_R_DOWN 0x36
#define SC_SHIFT_L_UP 0xAA
#define SC_SHIFT_R_UP 0xB6
#define SC_CAPS_DOWN 0x3A
#define SC_CTRL_L_DOWN 0x1D
#define SC_CTRL_L_UP 0x9D
#define SC_ALT_L_DOWN 0x38
#define SC_ALT_L_UP 0xB8
#define SC_C_DOWN 0x2E

#define KBD_CHAR_CTRL_U (1)
#define KBD_CHAR_CTRL_L (12)

struct TTY_IOQUEUE keyboard_ioq;

static const char keymap[2][128] = {
    {0,   0x1b, '1',  '2', '3',  '4', '5', '6', '7', '8', '9', '0', '-',
     '=', 0x08, '\t', 'q', 'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
     '[', ']',  '\n', 0,   'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
     ';', '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
     '.', '/',  0,    '*', 0,    ' ', 0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0},
    {0,   0x1b, '!',  '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
     '+', 0x08, '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
     '{', '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
     ':', '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
     '>', '?',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0}};

static uint8_t shift = 0;
static uint8_t caps = 0;
static uint8_t ctrl = 0;
static uint8_t alt = 0;

static kbd_gui_hook_t gui_hook = 0;

void keyboard_set_gui_hook(kbd_gui_hook_t hook) {
    gui_hook = hook;
}

char keyboard_translate(uint8_t scancode, int shifted) {
    if (scancode >= 128)
        return 0;
    char c = keymap[shifted ? 1 : 0][scancode];
    if (caps && c >= 'a' && c <= 'z')
        c -= 32;
    return c;
}

void keyboard_handler(void) {
    if (!(inb(KEYBOARD_STATUS) & 0x01)) {
        return;
    }
    uint8_t sc = inb(KEYBOARD_DATA);

    if (sc == SC_SHIFT_L_DOWN || sc == SC_SHIFT_R_DOWN) {
        shift = 1;
    } else if (sc == SC_SHIFT_L_UP || sc == SC_SHIFT_R_UP) {
        shift = 0;
    } else if (sc == SC_CAPS_DOWN) {
        caps = !caps;
    } else if (sc == SC_CTRL_L_DOWN) {
        ctrl = 1;
    } else if (sc == SC_CTRL_L_UP) {
        ctrl = 0;
    } else if (sc == SC_ALT_L_DOWN) {
        alt = 1;
    } else if (sc == SC_ALT_L_UP) {
        alt = 0;
    }

    if (gui_hook) {
        uint8_t mods = 0;
        if (shift)
            mods |= KBD_MOD_SHIFT;
        if (ctrl)
            mods |= KBD_MOD_CTRL;
        if (alt)
            mods |= KBD_MOD_ALT;
        gui_hook((uint8_t)(sc & 0x7F), !(sc & 0x80), mods);
        return;
    }

    if (sc == SC_SHIFT_L_DOWN || sc == SC_SHIFT_R_DOWN)
        return;
    if (sc == SC_SHIFT_L_UP || sc == SC_SHIFT_R_UP)
        return;
    if (sc == SC_CAPS_DOWN)
        return;
    if (sc == SC_CTRL_L_DOWN || sc == SC_CTRL_L_UP)
        return;
    if (sc == SC_ALT_L_DOWN || sc == SC_ALT_L_UP)
        return;
    if (sc & 0x80) {
        return;
    }
    if (sc >= 128) {
        return;
    }

    if (ctrl && sc == SC_C_DOWN) {
        tty_sigint_foreground();
        return;
    }

    if (ctrl && sc < 0x3b) {
        char c = 0;
        if (sc == KBD_SC_U)
            c = (char)KBD_CHAR_CTRL_U;
        else if (sc == KBD_SC_L)
            c = (char)KBD_CHAR_CTRL_L;
        if (c) {
            if (!ioq_full(&keyboard_ioq))
                ioq_putchar(&keyboard_ioq, c);
            return;
        }
    }

    char c = keymap[shift ? 1 : 0][sc];
    if (caps && c >= 'a' && c <= 'z')
        c -= 32;

    if (c && !ioq_full(&keyboard_ioq)) {
        ioq_putchar(&keyboard_ioq, c);
    }
}
