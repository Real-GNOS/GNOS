#include "libc/user/stdio.h"
#include "syscall.h"

int main(void) {
    char buf[256];
    char *r = getcwd(buf, sizeof buf);
    printf("getcwd(256): %s\n", r ? r : "(NULL)");

    char tiny[4];
    r = getcwd(tiny, 4);
    printf("getcwd(4): %s\n", r ? r : "(NULL)");

    char one[1];
    r = getcwd(one, 1);
    printf("getcwd(1): %s\n", r ? r : "(NULL)");

    r = getcwd((char *)0, 16);
    printf("getcwd(NULL): %s\n", r ? r : "(NULL)");
    return 0;
}
