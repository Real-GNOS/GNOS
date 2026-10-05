#include "kernel/syscall/linux_compat.h"
#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "drivers/char/ioqueue.h"
#include "drivers/char/keyboard.h"
#include "drivers/char/rtc.h"
#include "drivers/char/tty.h"
#include "drivers/char/pty.h"
#include "drivers/net/socket.h"
#include "kernel/asm_func.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/fsapi.h"
#include "kernel/fs/file.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/proc.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/access.h"
#include "kernel/sched/thread.h"
#include "kernel/shell/pipe.h"
#include "kernel/signal.h"
#include "kernel/syscall/file_syscall.h"
#include "kernel/syscall/futex.h"
#include "kernel/syscall/mmap.h"
#include "kernel/userprog/clone.h"
#include "kernel/userprog/exec.h"
#include "kernel/userprog/fork.h"
#include "kernel/userprog/process.h"
#include "kernel/userprog/wait_exit.h"
#include "lib/rand/rand.h"
#include "lib/str/str.h"
#include "libc/user/syscall.h"
#include "kernel/syscall/lc_internal.h"

#define UNIX_FD_BASE 0x400
#define MAX_UNIX_SOCK 16
#define UNIX_BUF_SIZE 2048

static struct {
    uint8_t active;
    uint8_t connected;
    uint8_t type;
    uint8_t nonblock;
    int8_t peer;
    int8_t self;
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint8_t buf[UNIX_BUF_SIZE];
} u_unix[MAX_UNIX_SOCK];

void unix_close_slot(int idx) {
    if (idx < 0 || idx >= MAX_UNIX_SOCK)
        return;
    u_unix[idx].active = 0;
    u_unix[idx].count = 0;
    u_unix[idx].head = 0;
    u_unix[idx].tail = 0;
}

int unix_fd_slot(uint64_t fd) {
    uint64_t idx = fd - UNIX_FD_BASE;
    if (idx >= MAX_UNIX_SOCK || !u_unix[idx].active)
        return -1;
    return (int)idx;
}
int unix_alloc_slot(void) {
    for (int i = 0; i < MAX_UNIX_SOCK; i++) {
        if (!u_unix[i].active)
            return i;
    }
    return -1;
}
uint32_t unix_buf_write(int idx, const uint8_t *src, uint32_t len) {
    uint32_t done = 0;
    while (done < len && u_unix[idx].count < UNIX_BUF_SIZE) {
        u_unix[idx].buf[u_unix[idx].tail] = src[done];
        u_unix[idx].tail =
            (uint16_t)((u_unix[idx].tail + 1) % UNIX_BUF_SIZE);
        u_unix[idx].count++;
        done++;
    }
    return done;
}
uint32_t unix_buf_read(int idx, uint8_t *dst, uint32_t len) {
    uint32_t done = 0;
    while (done < len && u_unix[idx].count > 0) {
        dst[done] = u_unix[idx].buf[u_unix[idx].head];
        u_unix[idx].head =
            (uint16_t)((u_unix[idx].head + 1) % UNIX_BUF_SIZE);
        u_unix[idx].count--;
        done++;
    }
    return done;
}
int unix_peer_alive(int idx) {
    int8_t p = u_unix[idx].peer;
    return p >= 0 && u_unix[p].active;
}
int32_t unix_send(int idx, const void *buf, uint32_t len) {
    if (!u_unix[idx].connected || u_unix[idx].peer < 0)
        return -LINUX_ENOTCONN;
    int p = u_unix[idx].peer;
    if (!u_unix[p].active)
        return -LINUX_EPIPE;
    for (;;) {
        uint32_t n = unix_buf_write(p, (const uint8_t *)buf, len);
        if (n > 0 || len == 0)
            return (int32_t)n;
        if (u_unix[idx].nonblock)
            return -LINUX_EAGAIN;
        mtime_sleep(1);
    }
}
int32_t unix_recv(int idx, void *buf, uint32_t len) {
    if (u_unix[idx].count == 0) {
        if (len == 0)
            return 0;
        while (u_unix[idx].count == 0) {
            if (!unix_peer_alive(idx))
                return 0;
            if (u_unix[idx].nonblock)
                return -LINUX_EAGAIN;
            mtime_sleep(1);
        }
    }
    return (int32_t)unix_buf_read(idx, (uint8_t *)buf, len);
}
int64_t lc_socket(LC_ARGS) {
    (void)r; (void)d; (void)e; (void)f;
    int domain = (int)a;
    int type = (int)b;
    if (domain == 1) {
        int i = unix_alloc_slot();
        if (i < 0)
            return -LINUX_ENFILE;
        memset(&u_unix[i], 0, sizeof(u_unix[i]));
        u_unix[i].active = 1;
        u_unix[i].connected = 0;
        u_unix[i].type = (uint8_t)type;
        u_unix[i].peer = -1;
        u_unix[i].self = (int8_t)i;
        return UNIX_FD_BASE + i;
    }
    if (domain != 2)
        return -LINUX_EAFNOSUPPORT;
    {
        int fd = net_socket(domain, type, (int)c);
        if (fd < 0)
            return -LINUX_ENFILE;
        return fd;
    }
}
int sockaddr_in_parts(struct ARCH_REGS *r, uint64_t addr,
                             uint64_t addrlen, uint32_t *ip,
                             uint16_t *port) {
    if (addr == 0 || addrlen < 8) {
        return -1;
    }

    uint8_t tmp[8];
    if (copy_from_user(tmp, (const void *)(uintptr_t)addr, sizeof(tmp)) != 0)
        return -1;
    uint16_t family = (uint16_t)(tmp[0] | ((uint16_t)tmp[1] << 8));
    if (family != 2)
        return -1;
    *port = (uint16_t)((uint16_t)(tmp[2] << 8) | tmp[3]);
    *ip = ((uint32_t)tmp[4]) | ((uint32_t)tmp[5] << 8) |
          ((uint32_t)tmp[6] << 16) | ((uint32_t)tmp[7] << 24);
    return 0;
}
int fill_sockaddr_in(struct ARCH_REGS *r, uint64_t addr,
                            uint64_t addrlen_ptr, uint32_t ip,
                            uint16_t port) {
    if (addr == 0 || addrlen_ptr == 0)
        return 0;

    uint32_t klen = 0;
    if (copy_from_user(&klen, (const void *)(uintptr_t)addrlen_ptr,
                       sizeof(klen)) != 0)
        return -1;
    uint8_t sa[8];
    sa[0] = 2;
    sa[1] = 0;
    sa[2] = (uint8_t)(port >> 8);
    sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)ip;
    sa[5] = (uint8_t)(ip >> 8);
    sa[6] = (uint8_t)(ip >> 16);
    sa[7] = (uint8_t)(ip >> 24);
    uint32_t n = (klen < sizeof(sa)) ? klen : (uint32_t)sizeof(sa);
    if (n != 0 && copy_to_user((void *)(uintptr_t)addr, sa, n) != 0)
        return -1;
    uint32_t alen = 16;
    if (copy_to_user((void *)(uintptr_t)addrlen_ptr, &alen, sizeof(alen)) != 0)
        return -1;
    return 0;
}
int64_t lc_connect(LC_ARGS) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOENT;
    uint32_t ip;
    uint16_t port;
    if (sockaddr_in_parts(r, b, c, &ip, &port) != 0)
        return -LINUX_EAFNOSUPPORT;
    return net_connect((int)a, ip, port);
}
int64_t lc_bind(LC_ARGS) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    uint32_t ip;
    uint16_t port;
    if (sockaddr_in_parts(r, b, c, &ip, &port) != 0)
        return -LINUX_EAFNOSUPPORT;
    return net_bind((int)a, ip, port);
}
int64_t lc_listen(LC_ARGS) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    return net_listen((int)a, (int)b);
}
int64_t lc_accept(LC_ARGS) {
    (void)r; (void)b; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_EAGAIN;
    return net_accept((int)a);
}
int64_t lc_shutdown(LC_ARGS) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;
    return net_shutdown((int)a, (int)b);
}
int64_t lc_sendto(LC_ARGS) {
    (void)d;
    int uslot = unix_fd_slot(a);
    if (uslot >= 0) {
        if (!user_ptr_ok(r, b, (uint32_t)c, 0))
            return -LINUX_EFAULT;
        return unix_send(uslot, (const void *)(uintptr_t)b, (uint32_t)c);
    }
    if (!user_ptr_ok(r, b, (uint32_t)c, 0))
        return -LINUX_EFAULT;
    if (e != 0) {
        uint32_t ip;
        uint16_t port;
        if (sockaddr_in_parts(r, e, f, &ip, &port) != 0)
            return -LINUX_EAFNOSUPPORT;
        return net_sendto((int)a, (const void *)(uintptr_t)b, (uint32_t)c,
                          ip, port);
    }
    return net_send((int)a, (const void *)(uintptr_t)b, (uint32_t)c);
}
int64_t lc_recvfrom(LC_ARGS) {
    (void)d;
    int uslot = unix_fd_slot(a);
    if (uslot >= 0) {
        if (!user_ptr_ok(r, b, (uint32_t)c, 1))
            return -LINUX_EFAULT;
        return unix_recv(uslot, (void *)(uintptr_t)b, (uint32_t)c);
    }
    if (!user_ptr_ok(r, b, (uint32_t)c, 1))
        return -LINUX_EFAULT;
    uint32_t sip = 0;
    uint16_t sport = 0;
    int n = net_recvfrom((int)a, (void *)(uintptr_t)b, (uint32_t)c, &sip,
                         &sport);
    if (n < 0)
        return -LINUX_EAGAIN;
    if (e != 0 &&
        fill_sockaddr_in(r, e, f, sip, sport) != 0)
        return -LINUX_EFAULT;
    return n;
}
int64_t lc_getsockname(LC_ARGS) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_EINVAL;
    uint32_t ip = 0;
    uint16_t port = 0;
    if (net_getsockname((int)a, &ip, &port) != 0)
        return -LINUX_ENOTSOCK;
    if (fill_sockaddr_in(r, b, c, ip, port) != 0)
        return -LINUX_EFAULT;
    return 0;
}
int64_t lc_getpeername(LC_ARGS) {
    (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOTCONN;
    uint32_t ip = 0;
    uint16_t port = 0;
    if (net_getpeername((int)a, &ip, &port) != 0)
        return -LINUX_ENOTSOCK;
    if (fill_sockaddr_in(r, b, c, ip, port) != 0)
        return -LINUX_EFAULT;
    return 0;
}
int64_t lc_setsockopt(LC_ARGS) {
    (void)f;
    if (unix_fd_slot(a) >= 0)
        return 0;

    if (d != 0 && !user_ptr_ok(r, d, (uint32_t)e, 0))
        return -LINUX_EFAULT;
    return net_setsockopt((int)a, (int)b, (int)c, (const void *)(uintptr_t)d,
                          (uint32_t)e);
}
int64_t lc_getsockopt(LC_ARGS) {
    (void)f;
    uint32_t klen = 0;
    uint32_t kval = 0;
    if (e != 0 &&
        copy_from_user(&klen, (const void *)(uintptr_t)e, sizeof(klen)) != 0)
        return -LINUX_EFAULT;
    if (unix_fd_slot(a) >= 0) {
        if (d != 0) {
            if (klen < 4)
                return -LINUX_EINVAL;
            if (copy_to_user((void *)(uintptr_t)d, &kval, sizeof(kval)) != 0)
                return -LINUX_EFAULT;
        }
        return 0;
    }

    if (d != 0 && e != 0 && klen < 4)
        return -LINUX_EINVAL;
    if (net_getsockopt((int)a, (int)b, (int)c, d != 0 ? &kval : NULL,
                       e != 0 ? &klen : NULL) != 0)
        return -LINUX_EINVAL;
    if (d != 0 &&
        copy_to_user((void *)(uintptr_t)d, &kval, sizeof(kval)) != 0)
        return -LINUX_EFAULT;
    if (e != 0 &&
        copy_to_user((void *)(uintptr_t)e, &klen, sizeof(klen)) != 0)
        return -LINUX_EFAULT;
    return 0;
}
int64_t lc_sendmsg(LC_ARGS) {
    (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOTCONN;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_MSGHDR), 0))
        return -LINUX_EFAULT;
    struct LINUX_MSGHDR mh;
    memcpy(&mh, (const void *)(uintptr_t)b, sizeof mh);
    uint32_t ip = 0;
    uint16_t port = 0;
    int have_addr = 0;
    if (mh.name != 0 && mh.namelen >= 6) {
        if (sockaddr_in_parts(r, mh.name, mh.namelen, &ip, &port) != 0)
            return -LINUX_EAFNOSUPPORT;
        have_addr = 1;
    }
    uint8_t sbuf[2048];
    uint32_t total = 0;
    for (uint64_t i = 0; i < mh.iovlen && total < sizeof sbuf; i++) {
        uint64_t ent = mh.iov + i * sizeof(struct LINUX_IOVEC);
        if (!user_ptr_ok(r, ent, sizeof(struct LINUX_IOVEC), 0))
            return -LINUX_EFAULT;
        struct LINUX_IOVEC iv;
        memcpy(&iv, (const void *)(uintptr_t)ent, sizeof iv);
        uint32_t n = iv.iov_len > sizeof sbuf - total ? sizeof sbuf - total
                                                  : (uint32_t)iv.iov_len;
        if (n == 0)
            continue;
        if (!user_ptr_ok(r, (uint64_t)iv.iov_base, n, 0))
            return -LINUX_EFAULT;
        memcpy(sbuf + total, (const void *)(uintptr_t)iv.iov_base, n);
        total += n;
    }
    if (have_addr)
        return net_sendto((int)a, sbuf, total, ip, port);
    return net_send((int)a, sbuf, total);
}
int64_t lc_recvmsg(LC_ARGS) {
    (void)c; (void)d; (void)e; (void)f;
    if (unix_fd_slot(a) >= 0)
        return -LINUX_ENOTCONN;
    if (!user_ptr_ok(r, b, sizeof(struct LINUX_MSGHDR), 1))
        return -LINUX_EFAULT;
    struct LINUX_MSGHDR mh;
    memcpy(&mh, (const void *)(uintptr_t)b, sizeof mh);
    uint8_t rbuf[2048];
    uint32_t sip = 0;
    uint16_t sport = 0;
    int n = net_recvfrom((int)a, rbuf, sizeof rbuf, &sip, &sport);
    if (n < 0)
        return -LINUX_EAGAIN;
    uint32_t copied = 0;
    if (mh.iov != 0 && mh.iovlen > 0) {
        uint64_t ent = mh.iov;
        if (!user_ptr_ok(r, ent, sizeof(struct LINUX_IOVEC), 0))
            return -LINUX_EFAULT;
        struct LINUX_IOVEC iv;
        memcpy(&iv, (const void *)(uintptr_t)ent, sizeof iv);
        uint32_t n2 = iv.iov_len > (uint64_t)n ? (uint32_t)n : (uint32_t)iv.iov_len;
        if (n2 != 0 && !user_ptr_ok(r, (uint64_t)iv.iov_base, n2, 1))
            return -LINUX_EFAULT;
        if (n2 != 0)
            memcpy((void *)(uintptr_t)iv.iov_base, rbuf, n2);
        copied = n2;
    }
    if (mh.name != 0 && mh.namelen >= 6) {
        if (!user_ptr_ok(r, mh.name, 8, 1))
            return -LINUX_EFAULT;
        uint8_t sa[8];
        sa[0] = 2;
        sa[1] = 0;
        sa[2] = (uint8_t)(sport >> 8);
        sa[3] = (uint8_t)sport;
        sa[4] = (uint8_t)sip;
        sa[5] = (uint8_t)(sip >> 8);
        sa[6] = (uint8_t)(sip >> 16);
        sa[7] = (uint8_t)(sip >> 24);
        memcpy((void *)(uintptr_t)mh.name, sa, 8);
    }
    mh.namelen = 16;
    memcpy((void *)(uintptr_t)b, &mh, sizeof mh);
    return (int64_t)copied;
}
int64_t lc_socketpair(LC_ARGS) {
    (void)c;
    (void)e;
    (void)f;
    if (a != 1)
        return -LINUX_EAFNOSUPPORT;
    if (b != 1 && b != 2)
        return -LINUX_EINVAL;
    if (d == 0 || !user_ptr_ok(r, d, 8, 1))
        return -LINUX_EFAULT;
    int i0 = unix_alloc_slot();
    int i1 = -1;
    if (i0 >= 0)
        i1 = unix_alloc_slot();
    if (i0 < 0 || i1 < 0)
        return -LINUX_ENFILE;
    memset(&u_unix[i0], 0, sizeof(u_unix[i0]));
    memset(&u_unix[i1], 0, sizeof(u_unix[i1]));
    u_unix[i0].active = 1;
    u_unix[i1].active = 1;
    u_unix[i0].connected = 1;
    u_unix[i1].connected = 1;
    u_unix[i0].type = (uint8_t)b;
    u_unix[i1].type = (uint8_t)b;
    u_unix[i0].peer = (int8_t)i1;
    u_unix[i1].peer = (int8_t)i0;
    u_unix[i0].self = (int8_t)i0;
    u_unix[i1].self = (int8_t)i1;
    int32_t out[2];
    out[0] = UNIX_FD_BASE + i0;
    out[1] = UNIX_FD_BASE + i1;
    memcpy((void *)(uintptr_t)d, out, sizeof(out));
    return 0;
}

#define EVFD_BASE 0x800
#define EVFD_MAX 16
#define TFD_BASE 0x880
#define TFD_MAX 8
#define EPFD_BASE 0xB00
#define EPFD_MAX 8
#define EP_MAX_ITEMS 32
#define LC_POLL_MAX 256
#define LC_SEL_MAX_BYTES 128u
#define LC_SEL_MAX_FDS (LC_SEL_MAX_BYTES * 8u)

struct LC_EPITEM {
    int32_t fd;
    uint32_t events;
    uint64_t data;
};

static struct {
    uint8_t active;
    uint8_t nonblock;
    uint8_t sema;
    uint8_t pad;
    uint64_t count;
} u_evfd[EVFD_MAX];

static struct {
    uint8_t active;
    uint8_t nonblock;
    int32_t clkid;
    uint64_t next_ms;
    uint64_t interval_ms;
    uint64_t expirations;
} u_tfd[TFD_MAX];

static struct {
    uint8_t active;
    uint8_t nonblock;
    int n;
    struct LC_EPITEM items[EP_MAX_ITEMS];
} u_ep[EPFD_MAX];

uint64_t lc_now_ms(void) {
    return (uint64_t)tick * (uint64_t)(1000u / PIT_HZ);
}
int evfd_slot(int fd) {
    int i = fd - EVFD_BASE;
    return (i >= 0 && i < EVFD_MAX && u_evfd[i].active) ? i : -1;
}
int tfd_slot(int fd) {
    int i = fd - TFD_BASE;
    return (i >= 0 && i < TFD_MAX && u_tfd[i].active) ? i : -1;
}
int ep_slot(int fd) {
    int i = fd - EPFD_BASE;
    return (i >= 0 && i < EPFD_MAX && u_ep[i].active) ? i : -1;
}
int tfd_expired(int i) {
    if (u_tfd[i].next_ms == 0)
        return 0;
    return lc_now_ms() >= u_tfd[i].next_ms;
}
int lc_close_extra(int32_t fd) {
    int i = evfd_slot(fd);
    if (i >= 0) {
        u_evfd[i].active = 0;
        return 1;
    }
    i = tfd_slot(fd);
    if (i >= 0) {
        u_tfd[i].active = 0;
        return 1;
    }
    i = ep_slot(fd);
    if (i >= 0) {
        u_ep[i].active = 0;
        u_ep[i].n = 0;
        return 1;
    }
    return 0;
}
int io_is_file_fd(int fd) {
    if (fd < 0 || fd >= (int)MAX_FILES_OPEN_PER_PROC)
        return 0;
    uint32_t g = fd_owner_task()->fd_table[fd];
    if (g == (uint32_t)-1)
        return 0;
    if (fd < 3 && g == (uint32_t)fd)
        return 0;
    return 1;
}
int io_fd_events(int fd, int want_read, int want_write) {
    int rv = 0;
    int i;
    int want_err = want_read;

    if (fd < 0)
        return LINUX_POLLNVAL;

    i = evfd_slot(fd);
    if (i >= 0) {
        if (want_read && u_evfd[i].count > 0)
            rv |= LINUX_POLLIN;
        if (want_write && u_evfd[i].count < 0xFFFFFFFFFFFFFFFEull)
            rv |= LINUX_POLLOUT;
        return rv;
    }
    i = tfd_slot(fd);
    if (i >= 0) {
        if (want_read && tfd_expired(i))
            rv |= LINUX_POLLIN;
        if (want_write)
            rv |= LINUX_POLLOUT;
        return rv;
    }
    i = ep_slot(fd);
    if (i >= 0)
        return rv;
    i = unix_fd_slot((uint64_t)fd);
    if (i >= 0) {
        if (want_read && u_unix[i].count > 0)
            rv |= LINUX_POLLIN;
        if (u_unix[i].connected && !unix_peer_alive(i))
            rv |= LINUX_POLLHUP;
        if (want_write && unix_peer_alive(i))
            rv |= LINUX_POLLOUT;
        return rv;
    }
    if (fd < 3) {
        if (fd == 0) {
            if (want_read && TTY.avail() > 0)
                rv |= LINUX_POLLIN;
            if (want_write)
                rv |= LINUX_POLLOUT;
        } else {
            if (want_read)
                rv |= LINUX_POLLIN;
            if (want_write)
                rv |= LINUX_POLLOUT;
        }
        return rv;
    }
    if (io_is_file_fd(fd)) {
        if (is_pipe(fd)) {
            struct FILE *f = file_get(fd_local2global((uint32_t)fd));
            if (f != NULL && f->fd_inode != NULL) {
                uint32_t len = ioq_length((struct TTY_IOQUEUE *)f->fd_inode);
                if (f->fd_flag == PIPE_RD_FLAG) {
                    if (want_read && len > 0)
                        rv |= LINUX_POLLIN;
                    if (want_read && len == 0 &&
                        !pipe_end_alive(f->proc_aux))
                        rv |= LINUX_POLLHUP;
                } else {
                    if (want_write && len < BUFSIZE - 1)
                        rv |= LINUX_POLLOUT;
                    if (!pipe_end_alive(f->proc_aux))
                        rv |= LINUX_POLLERR;
                }
            }
            return rv;
        }
        if (want_read)
            rv |= LINUX_POLLIN;
        if (want_write)
            rv |= LINUX_POLLOUT;
        return rv;
    }
    if (net_is_socket(fd)) {
        int nr = net_poll_ready(fd, want_read, want_write);
        if (want_err && (nr & LINUX_POLLERR))
            rv |= LINUX_POLLERR;
        rv |= nr & (LINUX_POLLIN | LINUX_POLLOUT | LINUX_POLLHUP | LINUX_POLLNVAL);
        return rv;
    }
    return LINUX_POLLNVAL;
}
int io_wait(struct LINUX_POLLFD *fds, uint32_t n, int64_t timeout_ms) {
    uint64_t deadline = timeout_ms >= 0 ? lc_now_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        int ready = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (fds[i].fd < 0) {
                fds[i].revents = 0;
                continue;
            }
            int want_r = (fds[i].events & (LINUX_POLLIN | LINUX_POLLPRI)) != 0;
            int want_w = (fds[i].events & LINUX_POLLOUT) != 0;
            int rv = io_fd_events(fds[i].fd, want_r, want_w);
            fds[i].revents = (int16_t)rv;
            if (rv != 0)
                ready++;
        }
        if (ready > 0)
            return ready;
        if (timeout_ms >= 0 && lc_now_ms() >= deadline)
            return 0;
        mtime_sleep(1);
    }
}
int64_t lc_poll_common(struct ARCH_REGS *r, uint64_t ufds, int32_t nfds,
                              int64_t timeout_ms) {
    static struct LINUX_POLLFD pf[LC_POLL_MAX];
    if (nfds < 0 || nfds > LC_POLL_MAX)
        return -LINUX_EINVAL;
    if (nfds == 0) {
        if (timeout_ms > 0)
            mtime_sleep((uint32_t)timeout_ms);
        return 0;
    }
    uint32_t bytes = (uint32_t)nfds * (uint32_t)sizeof(struct LINUX_POLLFD);
    if (!user_ptr_ok(r, ufds, bytes, 1))
        return -LINUX_EFAULT;
    memcpy(pf, (const void *)(uintptr_t)ufds, bytes);
    int rc = io_wait(pf, (uint32_t)nfds, timeout_ms);
    memcpy((void *)(uintptr_t)ufds, pf, bytes);
    return rc;
}
int64_t lc_timespec_to_ms(struct ARCH_REGS *r, uint64_t ptr,
                                 int64_t *out) {
    if (ptr == 0) {
        *out = -1;
        return 0;
    }
    struct LINUX_TIMESPEC ts;
    if (!user_ptr_ok(r, ptr, sizeof(ts), 0))
        return -LINUX_EFAULT;
    memcpy(&ts, (const void *)(uintptr_t)ptr, sizeof(ts));
    *out = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (*out < 0)
        *out = 0;
    return 0;
}
int64_t lc_poll(LC_ARGS) {
    (void)d; (void)e; (void)f;
    return lc_poll_common(r, a, (int32_t)b, (int32_t)c);
}
int64_t lc_ppoll(LC_ARGS) {
    (void)d; (void)e; (void)f;
    int64_t tmo = -1;
    int64_t rc = lc_timespec_to_ms(r, c, &tmo);
    if (rc != 0)
        return rc;
    return lc_poll_common(r, a, (int32_t)b, tmo);
}
int lc_fdset_test(const uint8_t *set, int fd) {
    if (fd < 0 || (uint32_t)fd >= LC_SEL_MAX_FDS)
        return 0;
    return (set[fd / 8] >> (fd % 8)) & 1;
}
void lc_fdset_set(uint8_t *set, int fd) {
    if (fd < 0 || (uint32_t)fd >= LC_SEL_MAX_FDS)
        return;
    set[fd / 8] |= (uint8_t)(1u << (fd % 8));
}
void lc_fdset_clear_high(uint8_t *set, uint32_t bytes, int nfds) {
    uint32_t bit = (uint32_t)nfds;
    if (bytes == 0)
        return;
    if (bit % 8) {
        set[bit / 8] &= (uint8_t)((1u << (bit % 8)) - 1u);
        bit = (bit + 7) / 8 * 8;
    }
    for (uint32_t b = bit / 8; b < bytes; b++)
        set[b] = 0;
}
int64_t lc_select_common(struct ARCH_REGS *r, int32_t nfds, uint64_t rd,
                                uint64_t wr, uint64_t ex, int64_t timeout_ms) {
    static uint8_t sets[3][LC_SEL_MAX_BYTES];
    static struct LINUX_POLLFD pf[LC_SEL_MAX_FDS];
    if (nfds < 0 || nfds > (int32_t)LC_SEL_MAX_FDS)
        return -LINUX_EINVAL;
    uint32_t bytes = ((uint32_t)nfds + 7u) / 8u;
    uint64_t ptrs[3];
    ptrs[0] = rd;
    ptrs[1] = wr;
    ptrs[2] = ex;
    for (int k = 0; k < 3; k++) {
        memset(sets[k], 0, LC_SEL_MAX_BYTES);
        if (ptrs[k] == 0)
            continue;
        if (bytes != 0) {
            if (!user_ptr_ok(r, ptrs[k], bytes, 1))
                return -LINUX_EFAULT;
            memcpy(sets[k], (const void *)(uintptr_t)ptrs[k], bytes);
            lc_fdset_clear_high(sets[k], bytes, nfds);
        }
    }
    int n = 0;
    for (int fd = 0; fd < nfds && n < (int)LC_SEL_MAX_FDS; fd++) {
        int ev = 0;
        if (lc_fdset_test(sets[0], fd))
            ev |= LINUX_POLLIN;
        if (lc_fdset_test(sets[1], fd))
            ev |= LINUX_POLLOUT;
        if (lc_fdset_test(sets[2], fd))
            ev |= LINUX_POLLIN | LINUX_POLLPRI;
        if (ev == 0)
            continue;
        pf[n].fd = fd;
        pf[n].events = (int16_t)ev;
        pf[n].revents = 0;
        n++;
    }
    int rc = io_wait(pf, (uint32_t)n, timeout_ms);
    if (rc < 0)
        return rc;
    for (int k = 0; k < 3; k++) {
        if (ptrs[k] != 0 && bytes != 0)
            memset(sets[k], 0, bytes);
    }
    int badfd = 0;
    for (int i = 0; i < n; i++) {
        int fd = pf[i].fd;
        int rv = pf[i].revents;
        if (rv & LINUX_POLLNVAL)
            badfd = 1;
        if (rv & LINUX_POLLIN)
            lc_fdset_set(sets[0], fd);
        if (rv & LINUX_POLLOUT)
            lc_fdset_set(sets[1], fd);
        if (rv & (LINUX_POLLERR | LINUX_POLLHUP))
            lc_fdset_set(sets[2], fd);
    }
    for (int k = 0; k < 3; k++) {
        if (ptrs[k] != 0 && bytes != 0)
            memcpy((void *)(uintptr_t)ptrs[k], sets[k], bytes);
    }
    if (badfd && rc == 0)
        return -LINUX_EBADF;
    return rc;
}
int64_t lc_select(LC_ARGS) {
    (void)f;
    int64_t timeout_ms = -1;
    if (e != 0) {
        struct LINUX_TIMEVAL tv;
        if (!user_ptr_ok(r, e, sizeof(tv), 0))
            return -LINUX_EFAULT;
        memcpy(&tv, (const void *)(uintptr_t)e, sizeof(tv));
        timeout_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (timeout_ms < 0)
            timeout_ms = 0;
    }
    return lc_select_common(r, (int32_t)a, b, c, d, timeout_ms);
}
int64_t lc_pselect6(LC_ARGS) {
    (void)f;
    int64_t timeout_ms = -1;
    int64_t rc = lc_timespec_to_ms(r, e, &timeout_ms);
    if (rc != 0)
        return rc;
    return lc_select_common(r, (int32_t)a, b, c, d, timeout_ms);
}
int64_t lc_epoll_create1(LC_ARGS) {
    (void)r; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    for (int i = 0; i < EPFD_MAX; i++) {
        if (u_ep[i].active)
            continue;
        memset(&u_ep[i], 0, sizeof(u_ep[i]));
        u_ep[i].active = 1;
        return EPFD_BASE + i;
    }
    return -LINUX_ENFILE;
}
int64_t lc_epoll_ctl(LC_ARGS) {
    (void)e; (void)f;
    int ep = ep_slot((int)a);
    if (ep < 0)
        return -LINUX_EBADF;
    int op = (int)b;
    int fd = (int)c;
    if (op == LINUX_EPOLL_CTL_DEL) {
        for (int i = 0; i < u_ep[ep].n; i++) {
            if (u_ep[ep].items[i].fd != fd)
                continue;
            u_ep[ep].items[i] = u_ep[ep].items[u_ep[ep].n - 1];
            u_ep[ep].n--;
            return 0;
        }
        return -LINUX_ENOENT;
    }
    struct LINUX_EPOLL_EVENT ev;
    if (d == 0 || !user_ptr_ok(r, d, sizeof(ev), 0))
        return -LINUX_EFAULT;
    memcpy(&ev, (const void *)(uintptr_t)d, sizeof(ev));
    if (op == LINUX_EPOLL_CTL_ADD) {
        for (int i = 0; i < u_ep[ep].n; i++) {
            if (u_ep[ep].items[i].fd == fd)
                return -LINUX_EEXIST;
        }
        if (u_ep[ep].n >= EP_MAX_ITEMS)
            return -LINUX_ENOSPC;
        u_ep[ep].items[u_ep[ep].n].fd = fd;
        u_ep[ep].items[u_ep[ep].n].events = ev.events;
        u_ep[ep].items[u_ep[ep].n].data = ev.data;
        u_ep[ep].n++;
        return 0;
    }
    if (op == LINUX_EPOLL_CTL_MOD) {
        for (int i = 0; i < u_ep[ep].n; i++) {
            if (u_ep[ep].items[i].fd != fd)
                continue;
            u_ep[ep].items[i].events = ev.events;
            u_ep[ep].items[i].data = ev.data;
            return 0;
        }
        return -LINUX_ENOENT;
    }
    return -LINUX_EINVAL;
}
int64_t lc_epoll_wait_common(struct ARCH_REGS *r, uint64_t a, uint64_t b,
                                    uint64_t c, int64_t timeout_ms) {
    int ep = ep_slot((int)a);
    if (ep < 0)
        return -LINUX_EBADF;
    int32_t maxevents = (int32_t)c;
    if (maxevents <= 0)
        return -LINUX_EINVAL;
    if (b == 0 || !user_ptr_ok(r, b, (uint32_t)maxevents *
                                        (uint32_t)sizeof(struct LINUX_EPOLL_EVENT), 1))
        return -LINUX_EFAULT;
    uint64_t deadline = timeout_ms >= 0 ? lc_now_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        int out = 0;
        int n = u_ep[ep].n;
        for (int i = 0; i < n && out < maxevents; i++) {
            struct LC_EPITEM *it = &u_ep[ep].items[i];
            int want_r = (it->events & (LINUX_POLLIN | LINUX_POLLPRI)) != 0;
            int want_w = (it->events & LINUX_POLLOUT) != 0;
            int rv = io_fd_events(it->fd, want_r, want_w);
            if (rv == 0)
                continue;
            struct LINUX_EPOLL_EVENT ev;
            ev.events = (uint32_t)(rv & (it->events |
                                         LINUX_POLLERR | LINUX_POLLHUP));
            ev.data = it->data;
            memcpy((void *)(uintptr_t)(b +
                                       (uint64_t)out * sizeof(ev)),
                   &ev, sizeof(ev));
            out++;
        }
        if (out > 0)
            return out;
        if (timeout_ms >= 0 && lc_now_ms() >= deadline)
            return 0;
        mtime_sleep(1);
    }
}
int64_t lc_epoll_wait(LC_ARGS) {
    (void)e; (void)f;
    return lc_epoll_wait_common(r, a, b, c, (int32_t)d);
}
int64_t lc_epoll_pwait(LC_ARGS) {
    (void)e; (void)f;
    return lc_epoll_wait_common(r, a, b, c, (int32_t)d);
}
int64_t lc_eventfd2(LC_ARGS) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    for (int i = 0; i < EVFD_MAX; i++) {
        if (u_evfd[i].active)
            continue;
        memset(&u_evfd[i], 0, sizeof(u_evfd[i]));
        u_evfd[i].active = 1;
        u_evfd[i].count = a;
        u_evfd[i].sema = (b & LINUX_EFD_SEMAPHORE) ? 1 : 0;
        u_evfd[i].nonblock = (b & LINUX_EFD_NONBLOCK) ? 1 : 0;
        return EVFD_BASE + i;
    }
    return -LINUX_ENFILE;
}
int64_t lc_eventfd(LC_ARGS) {
    (void)b; (void)c; (void)d; (void)e; (void)f;
    return lc_eventfd2(r, a, 0, 0, 0, 0, 0);
}
int64_t lc_timerfd_create(LC_ARGS) {
    (void)r; (void)c; (void)d; (void)e; (void)f;
    for (int i = 0; i < TFD_MAX; i++) {
        if (u_tfd[i].active)
            continue;
        memset(&u_tfd[i], 0, sizeof(u_tfd[i]));
        u_tfd[i].active = 1;
        u_tfd[i].clkid = (int32_t)a;
        u_tfd[i].nonblock = (b & LINUX_TFD_NONBLOCK) ? 1 : 0;
        return TFD_BASE + i;
    }
    return -LINUX_ENFILE;
}
int64_t lc_timerfd_settime(LC_ARGS) {
    (void)e; (void)f;
    int i = tfd_slot((int)a);
    if (i < 0)
        return -LINUX_EBADF;
    struct LINUX_ITIMERSPEC its;
    if (c == 0 || !user_ptr_ok(r, c, sizeof(its), 0))
        return -LINUX_EFAULT;
    memcpy(&its, (const void *)(uintptr_t)c, sizeof(its));
    uint64_t interval = (uint64_t)its.it_interval.tv_sec * 1000u +
                        (uint64_t)(its.it_interval.tv_nsec / 1000000);
    uint64_t value = (uint64_t)its.it_value.tv_sec * 1000u +
                     (uint64_t)(its.it_value.tv_nsec / 1000000);
    if (d != 0) {
        if (!user_ptr_ok(r, d, sizeof(its), 1))
            return -LINUX_EFAULT;
        struct LINUX_ITIMERSPEC old;
        memset(&old, 0, sizeof(old));
        if (u_tfd[i].next_ms != 0) {
            uint64_t now = lc_now_ms();
            uint64_t left = u_tfd[i].next_ms > now ? u_tfd[i].next_ms - now : 0;
            old.it_value.tv_sec = (int64_t)(left / 1000u);
            old.it_value.tv_nsec = (int64_t)((left % 1000u) * 1000000u);
        }
        old.it_interval.tv_sec = (int64_t)(u_tfd[i].interval_ms / 1000u);
        old.it_interval.tv_nsec =
            (int64_t)((u_tfd[i].interval_ms % 1000u) * 1000000u);
        memcpy((void *)(uintptr_t)d, &old, sizeof(old));
    }
    u_tfd[i].interval_ms = interval;
    u_tfd[i].expirations = 0;
    if (value == 0) {
        u_tfd[i].next_ms = 0;
    } else if (b & LINUX_TFD_TIMER_ABSTIME) {
        u_tfd[i].next_ms = value;
    } else {
        u_tfd[i].next_ms = lc_now_ms() + value;
    }
    return 0;
}
int64_t lc_timerfd_gettime(LC_ARGS) {
    (void)c; (void)d; (void)e; (void)f;
    int i = tfd_slot((int)a);
    if (i < 0)
        return -LINUX_EBADF;
    if (b == 0 || !user_ptr_ok(r, b, sizeof(struct LINUX_ITIMERSPEC), 1))
        return -LINUX_EFAULT;
    struct LINUX_ITIMERSPEC its;
    memset(&its, 0, sizeof(its));
    if (u_tfd[i].next_ms != 0) {
        uint64_t now = lc_now_ms();
        uint64_t left = u_tfd[i].next_ms > now ? u_tfd[i].next_ms - now : 0;
        its.it_value.tv_sec = (int64_t)(left / 1000u);
        its.it_value.tv_nsec = (int64_t)((left % 1000u) * 1000000u);
    }
    its.it_interval.tv_sec = (int64_t)(u_tfd[i].interval_ms / 1000u);
    its.it_interval.tv_nsec =
        (int64_t)((u_tfd[i].interval_ms % 1000u) * 1000000u);
    memcpy((void *)(uintptr_t)b, &its, sizeof(its));
    return 0;
}
int64_t lc_timerfd_tick(int i) {
    if (u_tfd[i].next_ms == 0 || lc_now_ms() < u_tfd[i].next_ms)
        return 0;
    uint64_t now = lc_now_ms();
    if (u_tfd[i].interval_ms == 0) {
        u_tfd[i].next_ms = 0;
        u_tfd[i].expirations++;
        return 1;
    }
    uint64_t missed = (now - u_tfd[i].next_ms) / u_tfd[i].interval_ms + 1;
    u_tfd[i].expirations += missed;
    u_tfd[i].next_ms += missed * u_tfd[i].interval_ms;
    return 1;
}
int64_t lc_eventfd_read(int i, void *buf, uint32_t count) {
    if (count < 8)
        return -LINUX_EINVAL;
    while (u_evfd[i].count == 0) {
        if (u_evfd[i].nonblock)
            return -LINUX_EAGAIN;
        mtime_sleep(1);
    }
    uint64_t v = 1;
    if (!u_evfd[i].sema)
        v = u_evfd[i].count;
    u_evfd[i].count -= v;
    memcpy(buf, &v, 8);
    return 8;
}
int64_t lc_eventfd_write(int i, const void *buf, uint32_t count) {
    if (count < 8)
        return -LINUX_EINVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == 0xFFFFFFFFFFFFFFFFull)
        return -LINUX_EINVAL;
    if (u_evfd[i].count > 0xFFFFFFFFFFFFFFFEull - v)
        return -LINUX_EAGAIN;
    u_evfd[i].count += v;
    return 8;
}
int64_t lc_timerfd_read(int i, void *buf, uint32_t count) {
    if (count < 8)
        return -LINUX_EINVAL;
    while (!lc_timerfd_tick(i)) {
        if (u_tfd[i].nonblock)
            return -LINUX_EAGAIN;
        mtime_sleep(1);
    }
    uint64_t v = u_tfd[i].expirations;
    u_tfd[i].expirations = 0;
    memcpy(buf, &v, 8);
    return 8;
}
