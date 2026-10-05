#include "kernel/gui/x11.h"

#include "drivers/char/console/io.h"
#include "drivers/net/socket.h"
#include "kernel/sched/thread.h"
#include "lib/str/str.h"

#define X11_TCP_PORT 6000

static uint8_t rx_buf[2048];
static uint8_t tx_buf[4096];

static void x11_serve_conn(int cfd) {
    struct X11_CONN *conn = x11_conn_open();
    if (!conn) {
        net_close(cfd);
        return;
    }
    for (;;) {
        if (conn->dead)
            break;
        uint32_t n;
        while ((n = x11_conn_drain(conn, tx_buf, sizeof(tx_buf))) > 0) {
            if (net_send(cfd, tx_buf, n) < 0)
                break;
        }
        int got = net_recv(cfd, rx_buf, sizeof(rx_buf));
        if (got <= 0)
            break;
        if (x11_conn_feed(conn, rx_buf, (uint32_t)got) < 0)
            break;
    }
    x11_conn_close(conn);
    net_close(cfd);
    kprintf("x11gw: client disconnected\n");
}

static void x11_server_thread(void *arg) {
    (void)arg;
    int sfd = net_socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        kprintf("x11gw: socket failed\n");
        thread_exit_current();
        return;
    }
    if (net_bind(sfd, 0, X11_TCP_PORT) != 0) {
        kprintf("x11gw: bind %u failed\n", (unsigned)X11_TCP_PORT);
        net_close(sfd);
        thread_exit_current();
        return;
    }
    if (net_listen(sfd, 2) != 0) {
        kprintf("x11gw: listen failed\n");
        net_close(sfd);
        thread_exit_current();
        return;
    }
    kprintf("x11gw: listening on tcp/%u\n", (unsigned)X11_TCP_PORT);
    for (;;) {
        int cfd = net_accept(sfd);
        if (cfd < 0)
            continue;
        x11_serve_conn(cfd);
    }
}

void x11_server_start(void) {
    kernel_thread("x11srv", 4, x11_server_thread, 0, 0x1);
}
