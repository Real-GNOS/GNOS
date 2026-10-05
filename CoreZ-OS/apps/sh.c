#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SH_MAX_ARGS 32
#define SH_LINE_MAX 256

static int sh_exec_line(const char *line) {
    char buf[SH_LINE_MAX];
    size_t n = strlen(line);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, line, n);
    buf[n] = 0;

    char *argv[SH_MAX_ARGS];
    int argc = 0;
    char *p = buf;
    while (*p != 0 && argc < SH_MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        argv[argc++] = p;
        while (*p != 0 && *p != ' ' && *p != '\t' && *p != '\n' &&
               *p != '\r') {
            p++;
        }
        if (*p != 0) {
            *p = 0;
            p++;
        }
    }
    if (argc == 0) {
        return 0;
    }
    argv[argc] = 0;

    char path[SH_LINE_MAX];
    pid_t pid = fork();
    if (pid < 0) {
        return 127;
    }
    if (pid == 0) {
        const char *prog = argv[0];
        if (prog[0] == '/') {
            execv(prog, argv);
        } else {
            static const char *dirs[] = {"/bin/", "/usr/bin/", "/"};
            for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
                snprintf(path, sizeof(path), "%s%s", dirs[i], prog);
                execv(path, argv);
            }
        }
        fprintf(stderr, "sh: %s: not found\n", prog);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    return 128;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        return sh_exec_line(argv[2]);
    }
    if (argc >= 2) {
        char joined[SH_LINE_MAX];
        size_t used = 0;
        joined[0] = 0;
        for (int i = 1; i < argc; i++) {
            size_t l = strlen(argv[i]);
            if (used + l + 2 >= sizeof(joined)) {
                break;
            }
            if (used != 0) {
                joined[used++] = ' ';
            }
            memcpy(joined + used, argv[i], l);
            used += l;
            joined[used] = 0;
        }
        return sh_exec_line(joined);
    }
    char line[SH_LINE_MAX];
    for (;;) {
        char cwd[SH_LINE_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            strcpy(cwd, "/");
        }
        printf("[corez@corez %s]$ ", cwd);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }
        sh_exec_line(line);
    }
    return 0;
}
