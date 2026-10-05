#ifndef NIITAN_NET_SOCKET_H
#define NIITAN_NET_SOCKET_H

#include <stdint.h>

#define AF_INET 2

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define EPERM 1
#define EBADF 9
#define EAGAIN 11
#define EFAULT 14
#define EINVAL 22
#define ENOTSOCK 88
#define ENOTCONN 107
#define EINPROGRESS 115
#define EOPNOTSUPP 95

#define SEL_FD_SET_BYTES 128u
#define SEL_FD_SET_FDS (SEL_FD_SET_BYTES * 8u)

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9

#define O_NONBLOCK 0x800

#define NET_FDSET_WORDS 2

#define MAX_SOCKET 64
#define NET_FD_BASE 64
#define NET_FD_LIMIT (NET_FD_BASE + MAX_SOCKET)

#define POLLIN_R 0x01
#define POLLOUT_R 0x04
#define POLLERR_R 0x08
#define POLLHUP_R 0x10
#define POLLNVAL_R 0x20

void sock_init(void);
int net_socket(int domain, int type, int proto);
int net_bind(int fd, uint32_t ip, uint16_t port);
int net_listen(int fd, int backlog);
int net_connect(int fd, uint32_t ip, uint16_t port);
int net_send(int fd, const void *buf, uint32_t len);
int net_recv(int fd, void *buf, uint32_t len);
int net_sendto(int fd, const void *buf, uint32_t len, uint32_t daddr,
               uint16_t dport);
int net_recvfrom(int fd, void *buf, uint32_t len, uint32_t *saddr,
                 uint16_t *sport);
int net_accept(int fd);
int net_close(int fd);

int net_shutdown(int fd, int how);
int net_getsockname(int fd, uint32_t *ip, uint16_t *port);
int net_getpeername(int fd, uint32_t *ip, uint16_t *port);
int net_getsockopt(int fd, int level, int optname, void *val, uint32_t *len);
int net_setsockopt(int fd, int level, int optname, const void *val,
                   uint32_t len);
int net_fcntl(int fd, int cmd, uint32_t arg);
int net_is_socket(int fd);
int net_fd_index(int fd);
int net_poll_ready(int fd, int want_read, int want_write);
int net_select(int nfds, uint32_t *rfds, uint32_t *wfds, uint32_t *efds,
               int timeout_ms);

void sock_poll(void);

#endif
