#ifndef DRIVERS_CHAR_TTY_H
#define DRIVERS_CHAR_TTY_H

#include <stdint.h>

#define TTY_FLAG 0x5454u

#define TTY_NCC 32

#define TTY_INLCR  0x00000002u 
#define TTY_IGNCR  0x00000004u 
#define TTY_ICRNL  0x00000008u 
#define TTY_OPOST  0x00000001u
#define TTY_ONLCR  0x00000004u
#define TTY_CREAD  0x00000080u
#define TTY_CLOCAL 0x00008000u
#define TTY_ISIG   0x00000001u
#define TTY_ICANON 0x00000002u
#define TTY_ECHO   0x00000008u
#define TTY_ECHOE  0x00000010u
#define TTY_ECHOK  0x00000020u
#define TTY_ECHONL 0x00000040u
#define TTY_IEXTEN 0x00000100u

#define TTY_VINTR  0
#define TTY_VQUIT  1
#define TTY_VERASE 2
#define TTY_VKILL  3
#define TTY_VEOF   4
#define TTY_VTIME  5
#define TTY_VMIN   6

#define TTY_IECHO TTY_ECHO
#define TTY_IMIN  TTY_ICANON

struct TTY_TERMIOS {
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t c_cc[TTY_NCC];
    uint32_t ispeed;
    uint32_t ospeed;
};

struct TTY_OPS {
    int (*read)(char *buf, uint32_t n);
    int (*write)(const char *buf, uint32_t n);
    int (*ioctl)(uint32_t cmd, uint64_t arg);
    uint32_t (*avail)(void);
};

extern const struct TTY_OPS TTY;

void tty_init(void);
int tty_open(void);
uint32_t tty_pgid_of(uint32_t pid);
void tty_sigint_foreground(void);

#define TTY_IOCTL_TCGETS 0x5401u
#define TTY_IOCTL_TCSETS 0x5402u
#define TTY_IOCTL_TCSETSW 0x5403u
#define TTY_IOCTL_TCSETSF 0x5404u
#define TTY_IOCTL_TIOCGETD 0x5400u
#define TTY_IOCTL_TIOCGWINSZ 0x5413u
#define TTY_IOCTL_TIOCSWINSZ 0x5414u
#define TTY_IOCTL_TIOCGPGRP 0x540fu
#define TTY_IOCTL_TIOCSPGRP 0x5410u
#define TTY_IOCTL_TIOCSCTTY 0x540eu
#define TTY_IOCTL_FIONREAD 0x541bu

#endif /* DRIVERS_CHAR_TTY_H */
