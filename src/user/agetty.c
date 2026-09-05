/*
 * agetty.c — AEOS login shell with themed prompts. (GPLv2)
 *
 * Combined login + shell, inspired by the original AEOS AGeTTy.
 * Supports 5 prompt styles (Default, Posh, Linux SH, DOS, Kali).
 *
 * Usage: agetty [tty]
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#define MAX_INPUT  512
#define MAX_ARGS   64
#define HIST_SIZE  64
#define CFG_FILE   "/etc/agetty.conf"
#define PASSWD_DB  "/etc/shadow"
#define VERSION    "0.9.5"
#define OS_NAME    "AEOS v5.11"

/* ---- Prompt styles ---------------------------------------------------- */
#define STYLE_DEFAULT 1
#define STYLE_POSH    2
#define STYLE_LINUXSH 3
#define STYLE_DOS     4
#define STYLE_KALI    5

static int  prompt_style = STYLE_DEFAULT;
static char username[64] = {0};
static char cwd[256] = {0};

static void load_config(void)
{
    FILE *f = fopen(CFG_FILE, "r");
    if (!f)
        return;
    int s = STYLE_DEFAULT;
    if (fscanf(f, "%d", &s) == 1 && s >= 1 && s <= 5)
        prompt_style = s;
    fclose(f);
}

static void save_config(int style)
{
    FILE *f = fopen(CFG_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", style);
        fclose(f);
    }
}

/* ---- Authentication --------------------------------------------------- */
static int do_login(void)
{
    struct termios t;
    tcgetattr(0, &t);
    t.c_lflag |= ECHO;
    tcsetattr(0, TCSANOW, &t);

    for (int attempt = 0; attempt < 3; attempt++) {
        printf("%s login: ", OS_NAME);
        fflush(stdout);

        if (!fgets(username, sizeof username, stdin))
            return -1;
        char *nl = strchr(username, '\n');
        if (nl) *nl = 0;
        if (strlen(username) == 0) continue;

        /* read password with echo off */
        t.c_lflag &= ~ECHO;
        tcsetattr(0, TCSANOW, &t);

        printf("Password: ");
        fflush(stdout);

        char pass[128] = {0};
        if (!fgets(pass, sizeof pass, stdin)) {
            t.c_lflag |= ECHO;
            tcsetattr(0, TCSANOW, &t);
            return -1;
        }
        nl = strchr(pass, '\n');
        if (nl) *nl = 0;

        t.c_lflag |= ECHO;
        tcsetattr(0, TCSANOW, &t);

        /* check password against /etc/shadow */
        FILE *f = fopen(PASSWD_DB, "r");
        if (!f) {
            printf("No shadow database, allowing login.\n");
            return 0;
        }
        char line[512];
        int auth = 0;
        while (fgets(line, sizeof line, f)) {
            char *p = strchr(line, ':');
            if (!p) continue;
            *p = 0;
            if (strcmp(line, username) != 0) continue;
            /* skip to password field */
            char *hash = ++p;
            p = strchr(hash, ':');
            if (p) *p = 0;
            /* hash format: $id$salt$encrypted */
            if (strcmp(hash, "*") == 0 || strcmp(hash, "!") == 0) {
                auth = 0;
                break;
            }
            char *encrypted = crypt(pass, hash);
            if (encrypted && strcmp(encrypted, hash) == 0)
                auth = 1;
            break;
        }
        fclose(f);
        if (auth) return 0;
        printf("Login incorrect\n");
    }
    return -1;
}

/* ---- Shell builtins --------------------------------------------------- */
static void cmd_help(void)
{
    printf("Built-in commands:\n");
    printf("  exit       — logout\n");
    printf("  help       — show this help\n");
    printf("  ps         — list running processes\n");
    printf("  kill PID   — send SIGTERM to a process\n");
    printf("  ver        — show version\n");
    printf("  whoami     — show current user\n");
    printf("  pwd        — print working directory\n");
    printf("  cd DIR     — change directory\n");
    printf("  clear      — clear screen\n");
    printf("  bgidm      — launch BGIDM desktop\n");
    printf("  style N    — set prompt style (1-5)\n");
    printf("  cdufetch   — system info display\n");
    printf("  shutdown   — power off\n");
    printf("  reboot     — restart\n");
    printf("  passwd     — change password\n");
    printf("  ls / cat / mkdir / rm / echo — standard utils\n");
    printf("  Programs: press Ctrl-C to stop, Ctrl-Z to background\n");
}

static void cmd_cdufetch(void)
{
    const char *styles[] = { "", "Default", "Posh", "Linux SH", "DOS", "Kali" };
    int s = (prompt_style >= 1 && prompt_style <= 5) ? prompt_style : 1;

    /* get uptime from /proc/uptime */
    FILE *f = fopen("/proc/uptime", "r");
    double uptime = 0;
    if (f) { fscanf(f, "%lf", &uptime); fclose(f); }
    int hrs = (int)(uptime / 3600);
    int min = ((int)(uptime / 60)) % 60;

    /* get memory info */
    FILE *mf = fopen("/proc/meminfo", "r");
    long total_mem = 0, free_mem = 0;
    if (mf) {
        char line[256];
        while (fgets(line, sizeof line, mf)) {
            if (sscanf(line, "MemTotal: %ld kB", &total_mem) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld kB", &free_mem) == 1) continue;
        }
        fclose(mf);
    }

    printf("    /\\     -----    /---\\   /----- |  CPU: x86-64\n");
    printf("   /  \\    .        |   |   |      |  RAM: %ld MB / %ld MB\n",
           (total_mem - free_mem) / 1024, total_mem / 1024);
    printf("  /....\\   -----    |   |   \\----\\ |  Uptime: %dh %dm\n", hrs, min);
    printf(" /      \\  .        |   |        | |  OS: %s Build 8070\n", OS_NAME);
    printf("/        \\ -----    \\---/   -----/ |  WM: BGIDM 1.11.0\n");
    printf(" _   _                      _      |  Shell: AGeTTy %s\n", VERSION);
    printf("|_| |_| |_  _ |_       |_| | |     |  Theme: %s\n", styles[s]);
    printf("|   | | |_ |_ | |   \\/   |.|_|     |  Kernel: AEOS 0.1 (x86_64)\n");
}

static void cmd_style(const char *arg)
{
    int s = atoi(arg);
    if (s < 1 || s > 5) {
        printf("Usage: style <1-5>\n");
        printf("  1. Default:  root@AEOS:ROOTFS/ #\n");
        printf("  2. Posh:     root>ROOTFS/>\n");
        printf("  3. Linux SH: ROOTFS/ #\n");
        printf("  4. DOS:      ROOTFS:\\>\n");
        printf("  5. Kali:     ----(root@AEOS)-[ROOTFS/]\n");
        return;
    }
    prompt_style = s;
    save_config(s);
    printf("Prompt style set to %d. Reboot to apply.\n", s);
}

static void cmd_passwd(void)
{
    struct termios t;
    tcgetattr(0, &t);

    printf("Enter current password: ");
    t.c_lflag &= ~ECHO;
    tcsetattr(0, TCSANOW, &t);

    char old[128] = {0};
    fgets(old, sizeof old, stdin);
    char *nl = strchr(old, '\n');
    if (nl) *nl = 0;

    /* verify old password */
    FILE *f = fopen(PASSWD_DB, "r");
    if (!f) {
        printf("Cannot open shadow database.\n");
        t.c_lflag |= ECHO;
        tcsetattr(0, TCSANOW, &t);
        return;
    }
    char line[512];
    int auth = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, ':');
        if (!p) continue;
        *p = 0;
        if (strcmp(line, username) != 0) continue;
        char *hash = ++p;
        p = strchr(hash, ':');
        if (p) *p = 0;
        char *enc = crypt(old, hash);
        if (enc && strcmp(enc, hash) == 0) auth = 1;
        break;
    }
    fclose(f);

    if (!auth) {
        printf("Incorrect password.\n");
        t.c_lflag |= ECHO;
        tcsetattr(0, TCSANOW, &t);
        return;
    }

    printf("\nEnter new password: ");
    char new1[128] = {0};
    fgets(new1, sizeof new1, stdin);
    nl = strchr(new1, '\n');
    if (nl) *nl = 0;

    printf("Retype new password: ");
    char new2[128] = {0};
    fgets(new2, sizeof new2, stdin);
    nl = strchr(new2, '\n');
    if (nl) *nl = 0;

    t.c_lflag |= ECHO;
    tcsetattr(0, TCSANOW, &t);

    if (strcmp(new1, new2) != 0) {
        printf("Passwords don't match.\n");
        return;
    }
    if (strlen(new1) < 1) {
        printf("Password too short.\n");
        return;
    }

    /* generate new hash and update shadow */
    char salt[32];
    snprintf(salt, sizeof salt, "$6$%08x$", (unsigned)time(NULL));
    char *new_hash = crypt(new1, salt);

    /* rewrite shadow entry */
    FILE *fin = fopen(PASSWD_DB, "r");
    FILE *fout = fopen(PASSWD_DB ".tmp", "w");
    if (!fin || !fout) {
        printf("Error updating password database.\n");
        if (fin) fclose(fin);
        if (fout) fclose(fout);
        return;
    }
    while (fgets(line, sizeof line, fin)) {
        char *p = strchr(line, ':');
        if (p && strncmp(line, username, p - line) == 0 && (int)(p - line) == (int)strlen(username)) {
            fprintf(fout, "%s:%s:20000:0:99999:7:::\n", username, new_hash);
            /* skip old entry's remaining fields */
            while (fgets(line, sizeof line, fin)) {
                if (line[0] != '\n' && line[0] != ':')
                    fputs(line, fout);
            }
        } else {
            fputs(line, fout);
        }
    }
    fclose(fin);
    fclose(fout);
    rename(PASSWD_DB ".tmp", PASSWD_DB);
    printf("Password updated.\n");
}

/* ---- Prompt rendering ------------------------------------------------- */
static void print_prompt(void)
{
    if (getcwd(cwd, sizeof cwd) == NULL)
        strcpy(cwd, "/");

    /* strip /home/username prefix for display */
    char display_cwd[256];
    if (strncmp(cwd, "/home/", 6) == 0) {
        snprintf(display_cwd, sizeof display_cwd, "ROOTFS/%s", cwd + 6);
        char *slash = strchr(display_cwd + 7, '/');
        if (slash) {
            /* keep it short */
        }
    } else {
        snprintf(display_cwd, sizeof display_cwd, "%s", cwd);
    }

    switch (prompt_style) {
    case STYLE_DEFAULT:
        printf("\033[1;32mroot@AEOS51\033[0m:\033[1;34m%s\033[0m# ", display_cwd);
        break;
    case STYLE_POSH:
        printf("\033[47m\033[30m%s\033[0m>\033[1;34m%s\033[0m> ", username, display_cwd);
        break;
    case STYLE_LINUXSH:
        printf("\033[1;32m%s\033[0m # ", display_cwd);
        break;
    case STYLE_DOS:
        printf("%s\\>", display_cwd);
        break;
    case STYLE_KALI:
        printf("\033[1;31m----(\033[0m\033[1;37m%s@AEOS\033[0m\033[1;31m)-[\033[0m\033[1;34m%s\033[0m\033[1;31m]\033[0m\n", username, display_cwd);
        printf("|--# ");
        break;
    default:
        printf("root@AEOS:%s# ", display_cwd);
        break;
    }
    fflush(stdout);
}

/* ---- External command execution ---------------------------------------- */
static int run_external(const char *line)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* child: parse args and exec */
        char *args[MAX_ARGS];
        char buf[MAX_INPUT];
        strncpy(buf, line, sizeof buf);
        buf[sizeof buf - 1] = 0;

        int argc = 0;
        char *tok = strtok(buf, " \t\n");
        while (tok && argc < MAX_ARGS - 1) {
            args[argc++] = tok;
            tok = strtok(NULL, " \t\n");
        }
        args[argc] = NULL;
        if (argc == 0) _exit(0);

        execvp(args[0], args);
        fprintf(stderr, "aeos: %s: %s\n", args[0], strerror(errno));
        _exit(127);
    } else if (pid > 0) {
        int status;
        tcsetpgrp(0, pid);
        waitpid(pid, &status, WUNTRACED);
        tcsetpgrp(0, getpgrp());
        /* check if stopped (Ctrl-Z) */
        if (WIFSTOPPED(status)) {
            printf("[%d]+ Stopped\n", pid);
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return -1;
}

/* ---- Main shell loop -------------------------------------------------- */
static void shell_loop(void)
{
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    char input[MAX_INPUT];

    /* command history */
    char history[HIST_SIZE][MAX_INPUT];
    int hist_count = 0;

    while (1) {
        print_prompt();

        if (!fgets(input, sizeof input, stdin))
            break;

        /* strip newline */
        char *nl = strchr(input, '\n');
        if (nl) *nl = 0;

        /* skip empty */
        if (strlen(input) == 0) continue;

        /* save history */
        if (hist_count < HIST_SIZE) {
            strncpy(history[hist_count], input, MAX_INPUT);
            hist_count++;
        }

        /* parse command */
        char *cmd = input;
        char *arg = NULL;
        char *sp = strchr(input, ' ');
        if (sp) {
            *sp = 0;
            arg = sp + 1;
            /* skip leading spaces */
            while (*arg == ' ') arg++;
        }

        /* built-in commands */
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "logout") == 0) {
            break;
        } else if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "ver") == 0) {
            printf("AEOS %s\n", OS_NAME);
            printf("AGeTTy %s\n", VERSION);
            printf("Kernel: AEOS 0.1 (x86_64)\n");
        } else if (strcmp(cmd, "whoami") == 0) {
            printf("%s\n", username);
        } else if (strcmp(cmd, "pwd") == 0) {
            printf("%s\n", cwd);
        } else if (strcmp(cmd, "cd") == 0) {
            if (arg && *arg)
                chdir(arg);
            else
                chdir("/home");
        } else if (strcmp(cmd, "clear") == 0) {
            printf("\033[2J\033[H");
        } else if (strcmp(cmd, "cdufetch") == 0) {
            cmd_cdufetch();
        } else if (strcmp(cmd, "style") == 0) {
            if (arg && *arg)
                cmd_style(arg);
            else
                cmd_help();
        } else if (strcmp(cmd, "passwd") == 0) {
            cmd_passwd();
        } else if (strcmp(cmd, "bgidm") == 0) {
            /* try to launch BGIDM desktop */
            pid_t pid = fork();
            if (pid == 0) {
                execl("/bin/bgidm", "bgidm", NULL);
                fprintf(stderr, "aeos: bgidm: %s\n", strerror(errno));
                _exit(1);
            } else if (pid > 0) {
                waitpid(pid, NULL, 0);
            }
        } else if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) {
            sync();
            execl("/sbin/halt", "halt", "-p", NULL);
            execl("/bin/sh", "shutdown", "-h", "now", NULL);
        } else if (strcmp(cmd, "reboot") == 0) {
            sync();
            execl("/sbin/reboot", "reboot", NULL);
            execl("/bin/sh", "shutdown", "-r", "now", NULL);
        } else {
            /* external command */
            run_external(input);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* configure terminal */
    struct termios t;
    if (tcgetattr(0, &t) == 0) {
        t.c_iflag |= ICRNL | IXON | BRKINT;
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &t);
    }

    setenv("TERM", "linux", 1);

    /* load config */
    load_config();
    if (prompt_style < 1 || prompt_style > 5)
        prompt_style = STYLE_DEFAULT;

    /* login banner */
    printf("\n");
    printf("      A         EEEEEEEE     OOOOO            SSSS   555555       11      11  \n");
    printf("     A A        .           O     O          S       5          ..11    ..11  \n");
    printf("    A   A       .          O       O        S        5            11      11  \n");
    printf("   A.....A      EEEEEEEE   O       O        S        555555       11      11  \n");
    printf("  A       A     .          O       O        S             5       11      11  \n");
    printf(" A         A    .           O     O        S              5  OO   11      11  \n");
    printf("A           A   EEEEEEEE     OOOOO     SSSS          555555  OO ..11..  ..11..\n");
    printf("\n");
    printf("AEOS %s Build 8070 Kernel Patch 4 — Community Edition\n", VERSION);
    printf("Based on GNOS kernel (x86_64)\n");
    printf("\n");

    /* authenticate */
    if (do_login() < 0) {
        printf("Too many failed attempts.\n");
        return 1;
    }

    printf("Welcome, %s!\n", username);
    printf("Type 'help' for available commands.\n\n");

    /* enter shell */
    shell_loop();

    printf("Goodbye, %s.\n", username);
    return 0;
}
