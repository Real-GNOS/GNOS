#include "libc/user/stdio.h"
#include "lib/str/str.h"
#include "syscall.h"

#define O_RDONLY 0

int main(int argc, char **argv) {
    if (argc > 2 || argv[1] == NULL) {
        printf("cat: argument error\neg: cat filename\n");
        exit(-2);
    }

    if (argc == 1) {
        char buf[512] = {0};
        int32_t n = read(0, buf, sizeof(buf) - 1);
        if (n > 0) {
            write(1, buf, (uint32_t)n);
        }
        exit(0);
    }
    char abs_path[512] = {0};
    if (argv[1][0] != '/') {
        getcwd(abs_path, 512);
        strcat(abs_path, "/");
        strcat(abs_path, argv[1]);
    } else {
        strcpy(abs_path, argv[1]);
    }
    int fd = open(abs_path, O_RDONLY);
    if (fd == -1) {
        printf("cat: open %s failed.\n", argv[1]);
        exit(-1);
    }
    char buf[1024];
    int read_bytes = 0;
    for (;;) {
        read_bytes = read(fd, buf, sizeof(buf) - 1);
        if (read_bytes <= 0) {
            break;
        }
        write(1, buf, (uint32_t)read_bytes);
    }
    close(fd);
    exit(0);
    return 0;
}
