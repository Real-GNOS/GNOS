/*
 * spawntest.c — probe the fork+pipe handshake the spawn machinery needs.
 * (GPL-2.0)
 *
 * dbus's _dbus_full_duplex_pipe() and glib's g_spawn both build a
 * close-on-exec pipe, flip it nonblocking with fcntl(F_SETFL), and pass it
 * across fork().  A session startup that dies with "failed to set pipe
 * nonblocking: Bad file descriptor" is failing one of those steps on our
 * kernel; this program reproduces each step in isolation so the debugcon
 * shows exactly which one goes wrong.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void step(const char *what, int r)
{
    if (r < 0)
        printf("%-28s FAIL: %s\n", what, strerror(errno));
    else
        printf("%-28s ok (%d)\n", what, r);
}

int main(void)
{
    int p[2];

    step("pipe2(O_CLOEXEC)", pipe2(p, O_CLOEXEC));

    int fl = fcntl(p[0], F_GETFL);
    printf("F_GETFL(read)  = %#x %s\n", fl, fl < 0 ? strerror(errno) : "");
    step("F_SETFL(read)+NB", fcntl(p[0], F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK));

    fl = fcntl(p[1], F_GETFL);
    printf("F_GETFL(write) = %#x %s\n", fl, fl < 0 ? strerror(errno) : "");
    step("F_SETFL(write)+NB", fcntl(p[1], F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK));

    pid_t pid = fork();
    if (pid == 0) {
        /* The child reports what ITS view of the inherited pipe looks
         * like -- the spawn protocols all fcntl in the forked child. */
        int fl2 = fcntl(p[0], F_GETFL);
        printf("child F_GETFL = %#x %s\n", fl2, fl2 < 0 ? strerror(errno) : "");
        step("child F_SETFL+NB", fcntl(p[0], F_SETFL, (fl2 < 0 ? 0 : fl2) | O_NONBLOCK));
        _exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    printf("child status=%#x\n", st);

    /* The half of the handshake that tells the parent "exec went wrong":
     * write from the child side, read back -- pipes must carry data and
     * report EOF when the writer closes, which is what both dbus's
     * babysitter and glib's error report rely on. */
    const char *msg = "hi";
    step("write(pipe)", (int)write(p[1], msg, 2));
    char buf[8];
    step("read(pipe)", (int)read(p[0], buf, sizeof buf));
    close(p[1]);
    step("read(EOF)", (int)read(p[0], buf, sizeof buf));
    close(p[0]);

    puts("spawn-test done");
    return 0;
}
