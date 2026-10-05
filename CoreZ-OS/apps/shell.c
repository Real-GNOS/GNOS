#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/fs.h"
#include "syscall.h"

static char conf_path[128];

static void load_conf(void) {
    conf_path[0] = 0;
    int32_t fd = open("/shell.conf", 0);
    if (fd == -1) {
        return;
    }
    int32_t n = read(fd, conf_path, (uint32_t)sizeof(conf_path) - 1);
    close(fd);
    if (n <= 0) {
        conf_path[0] = 0;
        return;
    }
    conf_path[n] = 0;
    for (int32_t i = 0; i < n; i++) {
        if (conf_path[i] == '\n' || conf_path[i] == '\r') {
            conf_path[i] = 0;
            break;
        }
    }
}

int main(void) {
    for (;;) {
        load_conf();
        int32_t pid = fork();
        if (pid == 0) {
            if (conf_path[0]) {
                execv(conf_path, (const char *[]){conf_path, NULL});
                printf("shell: exec %s failed\n", conf_path);
            }
#if CONFIG_FISH
            execv("/fish.elf", (const char *[]){"/fish.elf", NULL});
            printf("shell: exec /fish.elf failed\n");
#endif
            execv("/init_sh.elf", (const char *[]){"/init_sh.elf", NULL});
            printf("shell: exec /init_sh.elf failed\n");
            exit(-1);
        }
        if (pid < 0) {
            printf("shell: fork failed, retry\n");
            continue;
        }
        int32_t status = 0;
        int32_t got = wait(&status);
        printf("\n[shell %d exited, status %d, restarting]\n", (int)got,
               (int)status);
    }
    return 0;
}
