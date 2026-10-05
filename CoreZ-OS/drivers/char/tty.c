#include "drivers/char/tty.h"

#include "drivers/driver_ops.h"

#include "drivers/char/console/io.h"
#include "arch/cpu.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "kernel/sched/thread.h"
#include "kernel/fs/file.h"
#include "kernel/signal.h"
#include "kernel/syscall/linux_abi.h"
#include "lib/str/str.h"

#define TTY_WINSZ_ROW 25
#define TTY_WINSZ_COL 80

#define KC_VERASE 2
#define KC_VKILL 3
#define KC_VEOF 4
#define KC_VINTR 0
#define KC_VQUIT 1
#define KC_VSTART 17
#define KC_VSTOP 19
#define KC_VSUSP 26
#define KF_ECHOE 0x10u
#define KF_ECHOK 0x20u
#define KF_CREAD 0x80u
#define KF_CLOCAL 0x8000u
#define KF_CS8 0x30u

static struct LINUX_TERMIOS tty_tios;
static uint16_t tty_ws[4] = {TTY_WINSZ_ROW, TTY_WINSZ_COL, 0, 0};
static uint32_t tty_pgrp;

#define TT_NONE 0
#define TT_ESC 1
#define TT_CSI 2
#define TT_OSC 3
#define TT_DCS 4

#define TT_REPLY_KITTY 1
#define TT_REPLY_XTVERSION 1
#define TT_REPLY_OSC11 1
#define TT_REPLY_TCAP 1

static uint8_t tt_state;
static uint8_t tt_pending;
static char tt_seq[64];
static uint32_t tt_seq_n;

static void tt_reply(const char *s) {
    while (*s)
        ioq_putchar(&keyboard_ioq, *s++);
}

static void tt_csi_dispatch(char final_c) {
    char params[8];
    uint32_t np = 0;
    uint8_t priv = 0;
    for (uint32_t i = 0; i < tt_seq_n; i++) {
        char ch = tt_seq[i];
        if (ch == '?')
            priv = 1;
        else if (np < sizeof(params))
            params[np++] = ch;
    }
    if (final_c == 'c') {
        tt_reply("\x1b[?1;2c");
        return;
    }
    if (final_c == 'n' && !priv && np == 1 && params[0] == '6') {
        tt_reply("\x1b[1;1R");
        return;
    }
    if (final_c == 'u' && priv) {
#if TT_REPLY_KITTY
        tt_reply("\x1b[?0u");
#endif
        return;
    }
    if (final_c == 'q' && np >= 1 && params[0] == '>') {
#if TT_REPLY_XTVERSION
        tt_reply("\x1bP>|corez-os tty\x1b\\");
#endif
        return;
    }
}

static void tt_osc_dispatch(void) {
#if TT_REPLY_OSC11
    if (tt_seq_n >= 4 && tt_seq[0] == '1' && tt_seq[1] == '1' &&
        tt_seq[2] == ';' && tt_seq[3] == '?')
        tt_reply("\x1b]11;rgb:0000/0000/0000\x1b\\");
#endif
}

static void tt_dcs_dispatch(void) {
#if TT_REPLY_TCAP
    if (tt_seq_n >= 3 && tt_seq[0] == '+' && tt_seq[1] == 'q') {
        tt_reply("\x1bP0+r");
        for (uint32_t i = 2; i < tt_seq_n; i++)
            ioq_putchar(&keyboard_ioq, tt_seq[i]);
        tt_reply("\x1b\\");
    }
#endif
}

static void tt_seq_push(char c) {
    if (tt_seq_n < sizeof(tt_seq))
        tt_seq[tt_seq_n++] = c;
}

static void tt_esc_flush(char c) {
    if (c == '\\') {
        if (tt_pending == TT_OSC)
            tt_osc_dispatch();
        else if (tt_pending == TT_DCS)
            tt_dcs_dispatch();
        tt_state = TT_NONE;
        tt_pending = TT_NONE;
        return;
    }
    tt_pending = TT_NONE;
    if (c == '[') {
        tt_state = TT_CSI;
        tt_seq_n = 0;
    } else if (c == ']') {
        tt_state = TT_OSC;
        tt_seq_n = 0;
    } else if (c == 'P') {
        tt_state = TT_DCS;
        tt_seq_n = 0;
    } else {
        tt_state = TT_NONE;
    }
}

static void tty_esc_feed(char c) {
    switch (tt_state) {
    case TT_NONE:
        if (c == 0x1b)
            tt_state = TT_ESC;
        return;
    case TT_ESC:
        tt_esc_flush(c);
        return;
    case TT_CSI:
        if (c == 0x1b) {
            tt_state = TT_ESC;
            return;
        }
        if ((c >= '0' && c <= '9') || c == ';' || c == '?' || c == '>' ||
            c == '=' || c == '$' || c == ' ' || c == '!') {
            tt_seq_push(c);
            return;
        }
        tt_state = TT_NONE;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            tt_csi_dispatch(c);
        return;
    case TT_OSC:
        if (c == 0x07) {
            tt_state = TT_NONE;
            tt_osc_dispatch();
            return;
        }
        tt_seq_push(c);
        if (c == 0x1b) {
            tt_pending = TT_OSC;
            tt_state = TT_ESC;
        }
        return;
    case TT_DCS:
        if (c == 0x1b) {
            tt_pending = TT_DCS;
            tt_state = TT_ESC;
            return;
        }
        tt_seq_push(c);
        return;
    default:
        tt_state = TT_NONE;
        return;
    }
}

static int tty_write(const char *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        tty_esc_feed(buf[i]);
    console_vt_write(buf, n);
    return (int)n;
}

static int tty_echo_on(void) {
    return (tty_tios.c_lflag & LINUX_ECHO) != 0;
}

static void tty_echo(char c) {
    if (!tty_echo_on())
        return;
    if (c == '\r')
        return;
    console_putc(c);
}

static char ioq_getchar_sync(struct TTY_IOQUEUE *q) {
    uint32_t f = cpu_eflags();
    cpu_cli();
    char c = ioq_getchar(q);
    cpu_set_eflags(f);
    return c;
}

static char line_buf[256];
static uint32_t line_len = 0;
static uint32_t line_pos = 0;
static int tty_read_line(char *buf, uint32_t n) {
    if (line_pos >= line_len) {
        uint32_t got = 0;
        for (;;) {
            char c = ioq_getchar_sync(&keyboard_ioq);
            if (c == '\r' && (tty_tios.c_iflag & LINUX_ICRNL))
                c = '\n';
            if (c == '\n') {
                tty_echo(c);
                if (got < (uint32_t)sizeof(line_buf))
                    line_buf[got++] = '\n';
                break;
            }
            if (c == '\b' || c == 0x7f) {
                if (got == 0) {
                    tty_echo('\a');
                    continue;
                }
                got--;
                tty_echo('\b');
                tty_echo(' ');
                tty_echo('\b');
                continue;
            }
            if (got + 1 >= (uint32_t)sizeof(line_buf)) {
                tty_echo('\a');
                continue;
            }
            line_buf[got++] = c;
            tty_echo(c);
        }
        line_len = got;
        line_pos = 0;
    }
    {
        uint32_t avail = line_len - line_pos;
        uint32_t take = (avail < n) ? avail : n;
        for (uint32_t i = 0; i < take; i++)
            buf[i] = line_buf[line_pos + i];
        line_pos += take;
        return (int)take;
    }
}

static int tty_read_raw(char *buf, uint32_t n) {
    uint32_t vmin = tty_tios.c_cc[LINUX_VMIN];
    if (vmin == 0)
        vmin = 1;
    uint32_t got = 0;
    while (got < n) {
        buf[got++] = ioq_getchar_sync(&keyboard_ioq);
        tty_echo((char)buf[got - 1]);
        if (got >= vmin)
            break;
    }
    return (int)got;
}

static int tty_read(char *buf, uint32_t n) {
    if (n == 0)
        return 0;
    if (tty_tios.c_lflag & LINUX_ICANON)
        return tty_read_line(buf, n);
    return tty_read_raw(buf, n);
}

static uint32_t tty_avail(void) { return ioq_length(&keyboard_ioq); }

static void tty_getwinsz(uint16_t *w) {
    w[0] = tty_ws[0];
    w[1] = tty_ws[1];
    w[2] = tty_ws[2];
    w[3] = tty_ws[3];
}

static int tty_ioctl(uint32_t cmd, uint64_t arg) {
    switch (cmd) {
    case TTY_IOCTL_TCGETS:
        if (arg)
            memcpy((void *)(uintptr_t)arg, &tty_tios, sizeof(tty_tios));
        return 0;
    case TTY_IOCTL_TCSETS:
    case TTY_IOCTL_TCSETSW:
    case TTY_IOCTL_TCSETSF:
        if (arg) {
            memcpy(&tty_tios, (const void *)(uintptr_t)arg, sizeof(tty_tios));
            line_len = line_pos = 0;
        }
        return 0;
    case TTY_IOCTL_TIOCGWINSZ:
        if (arg)
            tty_getwinsz((uint16_t *)(uintptr_t)arg);
        return 0;
    case TTY_IOCTL_TIOCSWINSZ:
        if (arg) {
            const uint16_t *w = (const uint16_t *)(uintptr_t)arg;
            uint16_t old_row = tty_ws[0], old_col = tty_ws[1];
            tty_ws[0] = w[0];
            tty_ws[1] = w[1];
            tty_ws[2] = w[2];
            tty_ws[3] = w[3];
            if (tty_pgrp && (tty_ws[0] != old_row || tty_ws[1] != old_col))
                sys_kill(-(int)tty_pgrp, SIGWINCH);
        }
        return 0;
    case TTY_IOCTL_TIOCSCTTY:
        tty_pgrp = current->pgid ? current->pgid : current->pid;
        return 0;
    case TTY_IOCTL_TIOCGPGRP:
        if (arg)
            *(uint32_t *)(uintptr_t)arg = tty_pgrp;
        return 0;
    case TTY_IOCTL_TIOCSPGRP:
        if (arg)
            tty_pgrp = *(const uint32_t *)(uintptr_t)arg;
        return 0;
    case TTY_IOCTL_FIONREAD:
        if (arg)
            *(uint32_t *)(uintptr_t)arg = ioq_length(&keyboard_ioq);
        return 0;
    default:
        return -1;
    }
}

int tty_open(void) { return fd_install(0); }

uint32_t tty_pgid_of(uint32_t pid) {
    (void)pid;
    return tty_pgrp;
}

void tty_sigint_foreground(void) {
    uint32_t pgid = tty_pgrp;
    if (pgid == 0) {
        if (foreground_pid == (uint32_t)-1)
            return;
        pgid = foreground_pid;
    }
    sys_kill(-(int)pgid, SIGINT);
}

const struct TTY_OPS TTY = {
    .read = tty_read,
    .write = tty_write,
    .ioctl = tty_ioctl,
    .avail = tty_avail,
};

static int tty_drv(void) {
    tty_init();
    return 0;
}

DRIVER_REGISTER("tty", 12, tty_drv);

void tty_init(void) {
    memset(&tty_tios, 0, sizeof(tty_tios));
    tty_tios.c_iflag = LINUX_ICRNL;
    tty_tios.c_oflag = 0; 
    tty_tios.c_cflag = KF_CREAD | KF_CLOCAL | KF_CS8;
    tty_tios.c_lflag =
        LINUX_ISIG | LINUX_ICANON | LINUX_ECHO | KF_ECHOE | KF_ECHOK;
    tty_tios.c_cc[KC_VINTR] = 0x03;
    tty_tios.c_cc[KC_VQUIT] = 0x1c;
    tty_tios.c_cc[KC_VERASE] = 0x7f;
    tty_tios.c_cc[KC_VKILL] = 0x15;
    tty_tios.c_cc[KC_VEOF] = 0x04;
    tty_tios.c_cc[KC_VSTART] = 0x11;
    tty_tios.c_cc[KC_VSTOP] = 0x13;
    tty_tios.c_cc[KC_VSUSP] = 0x1a;
    tty_tios.c_cc[LINUX_VMIN] = 1;
    tty_tios.c_cc[LINUX_VTIME] = 0;
    tty_tios.c_ispeed = 0xf;
    tty_tios.c_ospeed = 0xf;
}
