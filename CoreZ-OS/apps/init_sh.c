#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "kernel/fs/dir.h"
#include "kernel/fs/fs.h"
#include "syscall.h"

#define MAX_ARG_NR 16

#define KBD_CTRL_U 0x01
#define KBD_CTRL_L 0x0C

#define SYS_NR_PS 19u
#define SYS_NR_SHUTDOWN 58u
#define SYS_NR_SETFGPID 78u
#define SYS_NR_SMASH 79u

static int32_t syscall1(uint32_t nr, uint32_t a1) {
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a1) : "memory");
    return ret;
}

static char cmd_line[MAX_PATH_LEN];
static char final_path[MAX_PATH_LEN];
static char cwd_cache[64];
static char *argv_buf[MAX_ARG_NR];

static void str_copy(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = 0;
}

static uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static char *str_find(char *hay, char needle) {
    for (; *hay; hay++) {
        if (*hay == needle) {
            return hay;
        }
    }
    return NULL;
}

static void make_clear_abs_path(const char *path, char *out) {
    if (path[0] == '/') {
        str_copy(out, path);
        return;
    }
    str_copy(out, cwd_cache);
    uint32_t n = str_len(out);
    if (n != 0 && out[n - 1] != '/') {
        out[n++] = '/';
        out[n] = 0;
    }
    str_copy(out + n, path);
}

static void print_prompt(void) {
    printf("[corez@corez %s]$ ", cwd_cache);
}

static void shell_erase(void) {
    putchar('\b');
    putchar(' ');
    putchar('\b');
}

static void readline(char *buf, int32_t count) {
    char *pos = buf;
    while (read(0, pos, 1) != -1 && (pos - buf) < count) {
        if (*pos == '\n' || *pos == '\r') {
            *pos = 0;
            putchar('\n');
            return;
        }
        if (*pos == '\b') {
            if (pos > buf) {
                shell_erase();
                --pos;
                *pos = 0;
            }
            continue;
        }
        if (*pos == KBD_CTRL_L) {
            *pos = 0;
            clear();
            print_prompt();
            printf("%s", buf);
            continue;
        }
        if (*pos == KBD_CTRL_U) {
            while (pos > buf) {
                shell_erase();
                --pos;
            }
            *pos = 0;
            continue;
        }
        putchar(*pos);
        ++pos;
    }
    printf("readline: can't find enter_key, max %d chars\n", count - 1);
}

static int32_t cmd_parse(char *cmd_str, char **argv) {
    int32_t argc = 0;
    for (int32_t i = 0; i < MAX_ARG_NR; i++) {
        argv[i] = NULL;
    }
    char *next = cmd_str;
    while (*next) {
        while (*next == ' ') {
            ++next;
        }
        if (*next == 0) {
            break;
        }
        if (argc >= MAX_ARG_NR) {
            return -1;
        }
        argv[argc] = next;
        while (*next && *next != ' ') {
            ++next;
        }
        if (*next) {
            *next++ = 0;
        }
        ++argc;
    }
    return argc;
}

static void fill_zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static void buildin_ls(int32_t argc, char **argv) {
    char *pathname = NULL;
    struct FS_STAT file_stat;
    int long_info = 0;
    int arg_path_nr = 0;
    for (int32_t i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (str_eq(argv[i], "-l")) {
                long_info = 1;
            } else {
                printf("ls: invalid option %s\n", argv[i]);
                return;
            }
        } else if (arg_path_nr == 0) {
            pathname = argv[i];
            arg_path_nr = 1;
        } else {
            printf("ls: only support one path\n");
            return;
        }
    }
    char lpath[MAX_PATH_LEN];
    if (pathname == NULL) {
        str_copy(lpath, cwd_cache);
    } else {
        make_clear_abs_path(pathname, lpath);
    }
    if (stat(lpath, &file_stat) == -1) {
        printf("ls: cannot access %s: No such file or directory\n", lpath);
        return;
    }
    if (file_stat.st_filetype != FT_DIRECTORY) {
        if (long_info) {
            printf("-  %d  %d  %s\n", (int)file_stat.st_ino,
                   (int)file_stat.st_size, lpath);
        } else {
            printf("%s\n", lpath);
        }
        return;
    }
    struct FS_DIR *dir = opendir(lpath);
    if (dir == NULL) {
        printf("ls: cannot open directory %s\n", lpath);
        return;
    }
    struct FS_DIRENT *dir_e = NULL;
    char sub[MAX_PATH_LEN];
    uint32_t plen = str_len(lpath);
    for (uint32_t i = 0; i < plen; i++) {
        sub[i] = lpath[i];
    }
    if (sub[plen - 1] != '/') {
        sub[plen] = '/';
        ++plen;
        sub[plen] = 0;
    }
    rewinddir(dir);
    if (long_info) {
        while ((dir_e = readdir(dir))) {
            uint32_t n = plen;
            for (uint32_t i = 0; dir_e->filename[i]; i++) {
                sub[n++] = dir_e->filename[i];
            }
            sub[n] = 0;
            fill_zero(&file_stat, sizeof(file_stat));
            if (stat(sub, &file_stat) == -1) {
                printf("ls: cannot access %s\n", dir_e->filename);
                closedir(dir);
                return;
            }
            char ft = (file_stat.st_filetype == FT_DIRECTORY)
                          ? 'd'
                          : (file_stat.st_filetype == FT_CHARDEVICE ? 'c'
                                                                    : '-');
            printf("%c  %d  %d  %s\n", ft, (int)dir_e->i_no,
                   (int)file_stat.st_size, dir_e->filename);
        }
    } else {
        while ((dir_e = readdir(dir))) {
            printf("%s ", dir_e->filename);
        }
        printf("\n");
    }
    closedir(dir);
}

static int buildin_cd(int32_t argc, char **argv) {
    if (argc > 2) {
        printf("cd: only support 1 argument!\n");
        return -1;
    }
    if (argc == 1) {
        final_path[0] = '/';
        final_path[1] = 0;
    } else {
        make_clear_abs_path(argv[1], final_path);
    }
    if (chdir(final_path) == -1) {
        printf("cd: no such directory %s.\n", final_path);
        return -1;
    }
    return 0;
}

static int buildin_execute(int32_t argc, char **argv) {
    if (argc <= 0 || argv[0] == NULL) {
        return 0;
    }
    if (str_eq(argv[0], "ls")) {
        buildin_ls(argc, argv);
        return 0;
    }
    if (str_eq(argv[0], "cd")) {
        if (buildin_cd(argc, argv) == 0) {
            str_copy(cwd_cache, final_path);
        }
        return 0;
    }
    if (str_eq(argv[0], "pwd")) {
        printf("%s\n", cwd_cache);
        return 0;
    }
    if (str_eq(argv[0], "ps")) {
        syscall1(SYS_NR_PS, 0);
        return 0;
    }
    if (str_eq(argv[0], "clear")) {
        clear();
        return 0;
    }
    if (str_eq(argv[0], "mkdir")) {
        if (argc != 2) {
            printf("mkdir: only support one argument!\n");
            return 0;
        }
        make_clear_abs_path(argv[1], final_path);
        if (mkdir(final_path) == -1) {
            printf("mkdir: create directory %s failed.\n", final_path);
        }
        return 0;
    }
    if (str_eq(argv[0], "rmdir")) {
        if (argc != 2) {
            printf("rmdir: only support one argument!\n");
            return 0;
        }
        make_clear_abs_path(argv[1], final_path);
        if (rmdir(final_path) == -1) {
            printf("rmdir: remove directory %s failed.\n", final_path);
        }
        return 0;
    }
    if (str_eq(argv[0], "rm")) {
        if (argc != 2) {
            printf("rm: only support one argument!\n");
            return 0;
        }
        make_clear_abs_path(argv[1], final_path);
        if (unlink(final_path) == -1) {
            printf("rm: delete %s failed.\n", final_path);
        }
        return 0;
    }
    if (str_eq(argv[0], "gui")) {
        int32_t pid = fork();
        if (pid > 0) {
            syscall1(SYS_NR_SETFGPID, (uint32_t)pid);
            int32_t status = 0;
            int32_t child_pid = wait(&status);
            syscall1(SYS_NR_SETFGPID, (uint32_t)-1);
            printf("\n[prog %d exited, status %d]\n", (int)child_pid,
                   (int)status);
        } else if (pid == 0) {
            execv("/gui.elf", (const char *[]){"/gui.elf", NULL});
            printf("gui: exec failed.\n");
            exit(-1);
        } else {
            printf("gui: fork failed.\n");
        }
        return 0;
    }
    if (str_eq(argv[0], "shutdown")) {
        if (argc != 1) {
            printf("shutdown: no argument support!\n");
            return 0;
        }
        printf("Shutting down...\n");
        syscall1(SYS_NR_SHUTDOWN, 0);
        return 0;
    }
    if (str_eq(argv[0], "smash")) {
        syscall1(SYS_NR_SMASH, 0);
        return 0;
    }
    return -1;
}

static void cmd_execute(int32_t argc, char **argv) {
    if (argc <= 0 || argv[0] == NULL) {
        return;
    }
    if (buildin_execute(argc, argv) == 0) {
        return;
    }
    make_clear_abs_path(argv[0], final_path);
    char *prog_path = final_path;
    struct FS_STAT file_stat;
    fill_zero(&file_stat, sizeof(file_stat));
    if (stat(prog_path, &file_stat) == -1 && str_find(argv[0], '.') == NULL &&
        str_len(prog_path) + 4 < MAX_PATH_LEN) {
        char *tail = prog_path + str_len(prog_path);
        str_copy(tail, ".elf");
    }
    if (stat(prog_path, &file_stat) == -1) {
        printf("sh: cannot access %s: No such file or directory\n", argv[0]);
        return;
    }
    int32_t pid = fork();
    if (pid > 0) {
        syscall1(SYS_NR_SETFGPID, (uint32_t)pid);
        int32_t status = 0;
        int32_t child_pid = wait(&status);
        syscall1(SYS_NR_SETFGPID, (uint32_t)-1);
        printf("\n[prog %d exited, status %d]\n", (int)child_pid,
               (int)status);
    } else if (pid == 0) {
        execv(prog_path, (const char **)argv);
        printf("execv %s failed.\n", prog_path);
        exit(-1);
    } else {
        printf("fork failed.\n");
    }
}

static void run_line(char *line) {
    char *pipe_symbol = str_find(line, '|');
    if (pipe_symbol == NULL) {
        int32_t argc = cmd_parse(line, argv_buf);
        if (argc == -1) {
            printf("num of arguments exceed %d\n", MAX_ARG_NR);
            return;
        }
        cmd_execute(argc, argv_buf);
        return;
    }
    char *segments[MAX_ARG_NR];
    int nseg = 0;
    char *each_cmd = line;
    segments[nseg++] = each_cmd;
    while (nseg < MAX_ARG_NR && (pipe_symbol = str_find(each_cmd, '|'))) {
        *pipe_symbol = 0;
        each_cmd = pipe_symbol + 1;
        segments[nseg++] = each_cmd;
    }
    int32_t prev_read_fd = -1;
    for (int i = 0; i < nseg; i++) {
        int32_t wr_fd = -1;
        int32_t next_read_fd = -1;
        if (i < nseg - 1) {
            int32_t pfd[2] = {-1, -1};
            if (pipe(pfd) == -1) {
                printf("sh: pipe create failed.\n");
                break;
            }
            wr_fd = pfd[1];
            next_read_fd = pfd[0];
        }
        if (prev_read_fd != -1) {
            fd_redirect(0, (uint32_t)prev_read_fd);
        }
        if (wr_fd != -1) {
            fd_redirect(1, (uint32_t)wr_fd);
        }
        int32_t argc = cmd_parse(segments[i], argv_buf);
        if (argc != -1) {
            cmd_execute(argc, argv_buf);
        }
        if (wr_fd != -1) {
            fd_redirect(1, 1);
            close(wr_fd);
        }
        if (prev_read_fd != -1) {
            fd_redirect(0, 0);
            close(prev_read_fd);
        }
        prev_read_fd = next_read_fd;
    }
    if (prev_read_fd != -1) {
        close(prev_read_fd);
    }
    fd_redirect(0, 0);
    fd_redirect(1, 1);
}

static void autoexec(void) {
    int32_t fd = open("/autoexec", 0);
    if (fd == -1) {
        return;
    }
    char buf[MAX_PATH_LEN];
    int32_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = 0;
    char *p = buf;
    while (p) {
        char *nl = str_find(p, '\n');
        if (nl) {
            *nl = 0;
        }
        run_line(p);
        p = nl ? nl + 1 : NULL;
    }
}

int main(void) {
    cwd_cache[0] = '/';
    cwd_cache[1] = 0;
    autoexec();
    for (;;) {
        print_prompt();
        fill_zero(final_path, sizeof(final_path));
        fill_zero(cmd_line, sizeof(cmd_line));
        readline(cmd_line, MAX_PATH_LEN);
        if (cmd_line[0] == 0) {
            continue;
        }
        run_line(cmd_line);
    }
    return 0;
}
