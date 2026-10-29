/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tty_kbd.c — PS/2 keyboard: scancode set 1 to the line discipline. (GPLv2)
 *
 * The keyboard is one device shared by every terminal, so the modifier state
 * and the prefix state machines are global -- unlike the line-discipline
 * state, which is one copy per terminal.  IRQ 1 drains the 8042 and feeds
 * every byte through kbd_feed(); the active terminal is the one that receives
 * the decoded text.
 *
 * Two prefix bytes need care:
 *
 *   0xE0  the next byte is the second half of an extended sequence (arrow
 *         keys, edit keys, right-hand Ctrl/Alt).  Its make codes are turned
 *         into the ANSI escape sequences readline expects.
 *
 *   0xE1  the Pause/Break key, which is not a prefix plus one byte but a
 *         whole six-byte sequence: E1 1D 45 E1 9D C5.  The old tty.c only
 *         dropped the byte after E1, so 1D was seen as a left-Ctrl press and
 *         the modifier stayed latched until the trailing 9D.  Swallow the
 *         rest of the sequence here instead.
 */
#include <stdint.h>

#include "tty.h"
#include "tty_internal.h"
#include "input.h"
#include "idt.h"
#include "debugcon.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* The 8042 command port, separate from the keyboard data port. */
#define KBD_CMD     0x64
#define KBD_ENABLE  0xAE /* enable the first PS/2 port (keyboard) */
#define KBD_DISABLE 0xAD

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Modifier state.  The keyboard is one device shared by every terminal, so
 * unlike the discipline state this really is global. */
static int g_shift, g_caps, g_ctrl, g_alt;

/* Set after a 0xE0 prefix byte: the next scan code is the second half of an
 * extended sequence (cursor keys, edit keys, right-hand modifiers). */
static int g_ext;

/* Bytes still to swallow after an 0xE1 prefix.  Pause/Break is
 * E1 1D 45 E1 9D C5, so five follow the prefix. */
static int g_e1_skip;

/*
 * Extended (0xE0-prefixed) make codes that produce text.  Indexed by make
 * code; NULL means "nothing to type" (a bare modifier, a release, ...).  The
 * strings are the ANSI escape sequences a real terminal emits for the
 * cursor and editing keys, which is exactly what readline expects in raw
 * mode -- so the arrow keys move the cursor instead of printing garbage.
 */
static const char *const kbd_ext_map[128] = {
    [0x47] = "\033[1~", /* Home     */
    [0x48] = "\033[A",  /* Up       */
    [0x49] = "\033[5~", /* PageUp   */
    [0x4B] = "\033[D",  /* Left     */
    [0x4D] = "\033[C",  /* Right    */
    [0x4F] = "\033[4~", /* End      */
    [0x50] = "\033[B",  /* Down     */
    [0x51] = "\033[6~", /* PageDown */
    [0x52] = "\033[2~", /* Insert   */
    [0x53] = "\033[3~", /* Delete   */
};

/*
 * Scancode set 1, US layout.  Index = make code, 0 = no ASCII meaning.
 *
 * Enter produces CR and Backspace produces DEL, which is what a real
 * terminal sends.  Turning CR into NL is the line discipline's job (ICRNL),
 * and DEL is what c_cc[VERASE] defaults to -- so a program that clears ICRNL
 * really does see the carriage return, instead of us having decided for it.
 */
static const char kbd_map[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9',  '0', '-', '=',  0x7F,
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',  '[', ']', '\r', 0,
    'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z',
    'x',  'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*',  0,   ' ',
};

static const char kbd_map_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',  0x7F,
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0,
    'A',  'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|',  'Z',
    'X',  'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ',
};

/* ---- modifiers --------------------------------------------------------- */
/*
 * Ctrl-Alt-F1..F6.  Function keys F1..F6 are make codes 0x3B..0x40 in
 * scancode set 1, contiguous, so the terminal index is a subtraction.  This
 * is checked before the key is translated into text: the combination must
 * never reach the line discipline, or every switch would also type garbage
 * into whatever shell was listening.
 */
#define KEY_F1 0x3B
#define KEY_F6 0x40

static int vt_hotkey(uint8_t code)
{
    if (!g_ctrl || !g_alt)
        return 0;
    if (code < KEY_F1 || code > KEY_F6)
        return 0;
    tty_vt_switch(code - KEY_F1);
    return 1;
}

/*
 * Applied by tty_vt_switch(): the Ctrl-Alt that caused a switch is still
 * held, but its releases may arrive on another terminal or not at all.  Drop
 * the modifiers rather than risk leaving Ctrl latched on.  Caps Lock is a
 * lock, not a held key, so it survives a switch on purpose.
 */
void tty_kbd_reset_modifiers(void)
{
    g_shift = g_ctrl = g_alt = 0;
    g_ext                    = 0;
    g_e1_skip                = 0;
}

/* ---- scancode decoding ------------------------------------------------- */
/*
 * Decode one scancode into the line discipline (or into the modifier/VT
 * state).  Pulled out of kbd_irq so the IRQ handler can drain every byte the
 * 8042 has queued without re-reading the data port.
 */
static void kbd_feed(uint8_t sc)
{
    /* The evdev decoder runs first: it wants every byte, prefix included,
     * and keeps its own 0xE0 state -- see drivers/input/ps2/keyboard.c. */
    input_kbd_scancode(sc);

    /* Pause/Break: the whole six-byte sequence is swallowed.  The evdev
     * decoder above has already been given all of it. */
    if (g_e1_skip) {
        g_e1_skip--;
        return;
    }

    if (sc == 0xE1) {
        g_e1_skip = 5; /* 1D 45 E1 9D C5 */
        g_ext     = 0;
        return;
    }
    if (sc == 0xE0) {
        g_ext = 1;
        return;
    }

    int     release = sc & 0x80;
    uint8_t code    = (uint8_t)(sc & 0x7F);

    /* Second half of an extended sequence.  Modifier releases do nothing;
     * key releases are ignored; otherwise the make code yields an escape
     * sequence (or sets a right-hand modifier). */
    if (g_ext) {
        g_ext = 0;
        if (release) {
            /* Right-hand modifiers have to be *cleared* on release too, or
             * AltGr would leave g_alt stuck on and the next F-key would
             * silently switch terminals. */
            if (code == 0x1D)
                g_ctrl = 0;
            else if (code == 0x38)
                g_alt = 0;
            return;
        }
        if (code == 0x1D) { /* right control */
            g_ctrl = 1;
            return;
        }
        if (code == 0x38) { /* right alt */
            g_alt = 1;
            return;
        }
        const char *seq = kbd_ext_map[code];
        if (seq) {
            while (*seq)
                tty_input_char((uint8_t)*seq++);
        }
        return;
    }

    switch (code) {
    case 0x2A:
    case 0x36: /* left / right shift */
        g_shift = !release;
        return;
    case 0x1D: /* left control */
        g_ctrl = !release;
        return;
    case 0x38: /* left alt */
        g_alt = !release;
        return;
    case 0x3A: /* caps lock */
        if (!release)
            g_caps = !g_caps;
        return;
    }
    if (release)
        return;

    /* Terminal switching wins over typing, and consumes the key. */
    if (vt_hotkey(code))
        return;

    char c = g_shift ? kbd_map_shift[code] : kbd_map[code];
    if (!c)
        return;

    if (g_caps && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (g_caps && g_shift && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    if (g_ctrl && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
        c = (char)((c | 0x20) - 'a' + 1); /* Ctrl-A .. Ctrl-Z */
    else if (g_ctrl && c == '\\')
        c = 0x1C; /* Ctrl-\ */

    tty_input_char((uint8_t)c);
}

static void kbd_irq(regs_t *r)
{
    (void)r;

    /*
     * The 8042 raises IRQ 1 on output-buffer-full.  That edge can be left
     * pending after the byte was already drained elsewhere -- the init-time
     * drain below, or a coalesced IRQ on real hardware -- so the handler can
     * run with nothing to read.  Reading the data port then returns bus
     * garbage, and if that garbage decodes to a key while a modifier happens
     * to be stuck the line discipline gets a spurious, self-repeating
     * keystroke (the symptom: the cursor scrolls on its own, or a ^C storm).
     * Only touch the port when a byte is genuinely waiting, and drain every
     * byte the controller has queued so a make and its break are never
     * reordered into a stuck key.
     */
    if (!(inb(KBD_STATUS) & 1)) {
        dbg_puts("KBDIRQ: spurious (buffer empty)\n");
        return;
    }

    do {
        /* The 8042's data port is shared with the auxiliary (mouse) port.
         * Status bit 5 marks a byte the aux device produced: those belong
         * to the mouse driver's IRQ 12 drain, not to the keyboard, and
         * stealing one would corrupt the mouse packet stream.
         *
         * This must be `break`, not `continue`: the loop condition is the
         * output-buffer-full bit, which stays set while the aux byte sits
         * unread in the data port.  `continue` would spin forever inside
         * IRQ 1 with interrupts disabled, freezing the whole machine the
         * moment a mouse packet is pending and the user presses a key --
         * the "login 卡死" symptom.  Stopping here leaves the byte for
         * IRQ 12 (mouse_irq) to drain. */
        if (inb(KBD_STATUS) & 0x20)
            break;
        uint8_t sc = inb(KBD_DATA);
        kbd_feed(sc);
    } while (inb(KBD_STATUS) & 1);
}

/* ---- controller bring-up ----------------------------------------------- */
void tty_keyboard_init(void)
{
    g_shift = g_caps = g_ctrl = g_alt = 0;
    g_ext                             = 0;
    g_e1_skip                         = 0;

    /*
     * QEMU's emulated keyboard already works with no init, but real hardware
     * (and a legacy-emulated USB keyboard) can leave the port disabled or its
     * output buffer full of firmware leftovers.  Briefly wait for the
     * controller to accept a command, enable the keyboard port, then drain
     * its buffer so the first IRQ is a real keystroke.
     */
    while (inb(KBD_STATUS) & 0x02) /* wait for input buffer empty */
        ;
    outb(KBD_CMD, KBD_ENABLE); /* enable PS/2 port 1 (keyboard) */
    while (inb(KBD_STATUS) & 1)
        (void)inb(KBD_DATA);

    irq_install(1, kbd_irq, "keyboard");
}
