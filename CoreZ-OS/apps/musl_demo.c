#include <string.h>
#include <unistd.h>

static void put_ulong(int fd, unsigned long v) {
    char b[12];
    int i = 12;
    b[--i] = '\n';
    if (v == 0)
        b[--i] = '0';
    while (v) {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    }
    write(fd, b + i, (unsigned)(12 - i));
}

int main(void) {
    const char *tag = "musl getpid=";
    write(1, tag, strlen(tag));
    put_ulong(1, (unsigned long)getpid());

    const char *msg = "strlen=";
    write(1, msg, strlen(msg));

    char buf[16];
    strcpy(buf, "ok strcpy=");
    write(1, buf, strlen(buf));
    write(1, "ok", 2);
    write(1, "\n", 1);

    _exit(0);
    return 0;
}
