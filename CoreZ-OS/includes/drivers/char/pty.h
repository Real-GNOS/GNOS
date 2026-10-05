#ifndef DRIVERS_CHAR_PTY_H
#define DRIVERS_CHAR_PTY_H

#include "drivers/char/ioqueue.h"
#include <stdint.h>

#define NPTY 8
#define PTY_MASTER_MAJOR 136u
#define PTY_SLAVE_MAJOR 137u

#define PTY_TIOCGPTN 0x8004540Fu
#define PTY_TIOCSPTLCK 0x40045411u
#define PTY_TIOCSCTTY 0x540Eu
#define PTY_TIOCGPGRP 0x540fu
#define PTY_TIOCSPGRP 0x5410u
#define PTY_TIOCGWINSZ 0x5413u
#define PTY_TIOCSWINSZ 0x5414u

struct FILE;

struct PTY {
    int in_use;
    int master_open;
    int slave_open;
    int master_locked;
    int id;
    uint32_t pgrp;
    uint16_t ws[4];
    struct TTY_IOQUEUE m2s;
    struct TTY_IOQUEUE s2m;
};

int pty_chardev_open(struct FILE *f, uint32_t dev);
int32_t pty_chardev_read(struct FILE *f, void *buf, uint32_t count);
int32_t pty_chardev_write(struct FILE *f, const void *buf, uint32_t count);
int pty_chardev_ioctl(struct FILE *f, uint32_t cmd, uint64_t arg);
void pty_chardev_close(struct FILE *f);
int pty_has_priv(const struct FILE *f);

#endif /* DRIVERS_CHAR_PTY_H */
