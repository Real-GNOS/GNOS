#include "libc/user/stdio.h"
#include "lib/str/str.h"
#include "syscall.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int32_t fd[2] = {-1, -1};
    if (pipe(fd) == -1) {
        printf("prog_pipe: pipe create failed.\n");
        exit(-1);
    }
    int32_t pid = fork();
    if (pid > 0) {
        close(fd[0]);
        const char *msg = "Hi, my son, I love u!";
        uint32_t n = (uint32_t)strlen(msg);
        write(fd[1], msg, n);
        printf("\nI'm father, my pid is %d\n", getpid());
        exit(8);
    } else if (pid == 0) {
        close(fd[1]);
        char buf[64] = {0};
        int32_t n = read(fd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
        }
        printf("\nI'm child, my pid is %d\n", getpid());
        printf("I'm child, my father said to me: \"%s\"\n", buf);
        exit(9);
    } else {
        printf("prog_pipe: fork failed.\n");
        exit(-1);
    }
    return 0;
}
