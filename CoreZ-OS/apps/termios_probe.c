#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int checks, passed;
#define CK(c)                                                                                                              \
    do {                                                                                                                   \
        checks++;                                                                                                          \
        if (c)                                                                                                             \
            passed++;                                                                                                      \
    } while (0)

int main(void) {
    struct termios a, b, orig;
    struct winsize ws;
    char buf[64];
    int m;

    if (tcgetattr(STDIN_FILENO, &a) != 0) {
        write(2, "termios: tcgetattr FAIL\n", 24);
        _exit(2);
    }
    orig = a;
    CK(a.c_lflag & ICANON);
    CK(a.c_lflag & ECHO);
    CK(a.c_iflag & ICRNL);
    CK(a.c_cc[VMIN] == 1);

    b = a;
    b.c_lflag &= ~((tcflag_t)(ICANON | ECHO | ISIG | IEXTEN));
    b.c_iflag &= ~((tcflag_t)(ICRNL | BRKINT | INLCR | IGNCR | IXON));
    b.c_oflag &= ~OPOST;
    b.c_cc[VMIN] = 1;
    b.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &b) != 0) {
        write(2, "termios: tcsetattr FAIL\n", 24);
        _exit(3);
    }
    memset(&a, 0, sizeof a);
    tcgetattr(STDIN_FILENO, &a);
    CK(!(a.c_lflag & ICANON));
    CK(!(a.c_lflag & ECHO));
    CK(!(a.c_lflag & ISIG));
    CK(!(a.c_iflag & ICRNL));
    CK(a.c_cc[VMIN] == 1 && a.c_cc[VTIME] == 0);

    int wr = ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    CK(wr == 0);
    if (wr == 0) {
        CK(ws.ws_row == 25);
        CK(ws.ws_col == 80);
    }

    ws.ws_row = 40;
    ws.ws_col = 120;
    CK(ioctl(STDIN_FILENO, TIOCSWINSZ, &ws) == 0);
    memset(&ws, 0, sizeof ws);
    ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    CK(ws.ws_row == 40);
    CK(ws.ws_col == 120);

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    memset(&a, 0, sizeof a);
    tcgetattr(STDIN_FILENO, &a);
    CK((a.c_lflag & ICANON) && (a.c_lflag & ECHO));

    m = snprintf(buf, sizeof buf, "termios: %s (%d/%d)\n",
                 passed == checks ? "PASS" : "FAIL", passed, checks);
    write(1, buf, (size_t)m);
    _exit(passed == checks ? 0 : 1);
}
