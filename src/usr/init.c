/*
 * init.c — GNOS init, PID 1. (GPLv2)
 *
 * init is a supervisor, not a terminal.  It does exactly three things:
 *
 *   - makes itself immune to the terminal's signals, so that a stray Ctrl-C
 *     can never take the whole system down,
 *   - runs /etc/rc once, then starts a getty on each of the six virtual
 *     terminals, each in a session of its own,
 *   - reaps children forever, including the orphans the kernel re-parents
 *     onto it, and restarts whichever getty died.
 *
 * The getty layer is what makes this a multi-user system rather than a
 * single-user one with credentials bolted on.  init runs as root and must
 * stay that way -- it is the only process that can start a session -- so the
 * privilege drop has to happen somewhere downstream, and "somewhere" is
 * login(1), one exec later.  Each terminal therefore runs:
 *
 *     init (root) -> getty (root, owns /dev/ttyN) -> login (root, then not)
 *                 -> the user's shell
 *
 * Keeping each of those in a process of its own is not decoration: job
 * control is defined in terms of process groups and a controlling terminal,
 * and a session that never called setsid() has neither.
 */
#include "ulib.h"

#define GETTY_PATH  "/sbin/getty"  /* one per virtual terminal */
#define AGETTY_PATH "/sbin/agetty" /* GNOS login shell on tty1 */
#define FALLBACK_SH "/bin/bash"    /* if getty is missing entirely */
#define RC_SHELL    "/bin/sh"      /* the one-shot startup script */
#define RC_PATH     "/etc/rc"

#define NR_TTY      6              /* must match the kernel's NR_VT */

/* If the shell cannot even start, stop trying: an exec that fails instantly
 * in a restart loop is a fork bomb with extra steps. */
#define MAX_FAST_RESTARTS 5

static void ignore_terminal_signals(void)
{
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

static void default_terminal_signals(void)
{
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

/*
 * Run a single startup script through the shell and wait for it to finish.
 * It is its own process group but is not given the terminal: a script reads
 * its commands from a file, not the keyboard, so there is nothing to
 * foreground.  A missing script is harmless -- the shell reports it and the
 * boot goes on.
 */
static void run_rc(void)
{
    int pid = fork();
    if (pid < 0) {
        print("init: fork failed for rc\n");
        return;
    }

    if (pid == 0) {
        char *av[3];
        av[0] = (char *)RC_SHELL;
        av[1] = (char *)RC_PATH;
        av[2] = 0;

        /* A startup script must not read from or block on the keyboard, so
         * point its stdin at /dev/null; stdout/stderr stay on the console. */
        int nullin = sys_open("/dev/null", O_RDONLY);
        if (nullin >= 0 && nullin != 0) {
            sys_dup2(nullin, 0);
            sys_close(nullin);
        }

        setpgid(0, 0);
        default_terminal_signals();
        print("init: execing " RC_SHELL "\n");
        execv(RC_SHELL, av);
        print("init: execv " RC_SHELL " failed\n");
        exit(127);
    }

    /* The startup script is the boot's one foreground job: it owns the
     * terminal while it runs, so that the commands it spawns -- e.g. ttytest
     * reconfiguring the line discipline -- are not a *background* group and
     * thus do not get stopped with SIGTTOU for touching the tty.  init itself
     * ignores SIGTTOU, so this tcsetpgrp from a non-foreground process is safe. */
    setpgid(pid, pid);
    tcsetpgrp(0, pid);

    int status = 0;
    waitpid(pid, &status, 0);
    print("init: rc finished, status ");
    printn((long)status);
    print("\n");
}

/*
 * Start a getty on /dev/tty<n>.  The child does not touch the terminal here:
 * getty itself calls setsid() and TIOCSCTTY, which is the only way the new
 * session ends up owning the line rather than inheriting init's idea of it.
 * All we do is get out of its way -- close the descriptors we inherited from
 * the boot console and put the child in a group of its own.
 */
static int spawn_getty(int n)
{
    char name[8];
    name[0] = 't'; name[1] = 't'; name[2] = 'y';
    name[3] = (char)('0' + n);
    name[4] = 0;

    /* tty1 gets the GNOS login shell (AGeTTy) with themed prompts + BGIDM */
    const char *prog = (n == 1) ? AGETTY_PATH : GETTY_PATH;

    int pid = fork();
    if (pid < 0)
        return pid;

    if (pid == 0) {
        char *av[3];
        av[0] = (char *)prog;
        av[1] = name;
        av[2] = 0;

        setpgid(0, 0);
        default_terminal_signals();

        /* Our inherited stdin/stdout/stderr are terminal 1's.  getty opens
         * the terminal it was told to and dup2()s it over 0/1/2, but until it
         * does, an error message from the exec below would land on the wrong
         * screen -- so leave them alone rather than closing them, and let
         * getty overwrite them. */
        execv(prog, av);

        /* No getty in the image: fall back to a bare root shell on terminal
         * 1 so the machine is still usable, and do not loop on the others. */
        if (n == 1) {
            char *sh[2];
            sh[0] = (char *)FALLBACK_SH;
            sh[1] = 0;
            tcsetpgrp(0, getpid());
            execv(FALLBACK_SH, sh);
        }
        print("init: cannot exec ");
        print(prog);
        print("\n");
        exit(127);
    }

    setpgid(pid, pid);
    return pid;
}

/*
 * Single-user (recovery) mode: `single`, `S` or `-s` on init's command line
 * skips /etc/rc and drops to a root login shell on the console instead --
 * the boot stops at a bare bash, exactly like a classic init "s" runlevel.
 * The shell runs as the console's foreground job (the same arrangement
 * spawn_getty falls back to when no getty exists); when it exits, the boot
 * continues with rc and the normal gettys, like every other System V init.
 */
static void run_single_user(void)
{
    print("init: single-user mode: starting a root shell on the console\n");
    print("init: exit the shell to continue the normal boot\n");

    int pid = fork();
    if (pid < 0) {
        print("init: fork failed for single-user shell\n");
        return;
    }

    if (pid == 0) {
        char *av[2];
        av[0] = (char *)"-bash";       /* leading '-' => login shell */
        av[1] = 0;

        setpgid(0, 0);
        default_terminal_signals();
        execv(FALLBACK_SH, av);
        print("init: execv " FALLBACK_SH " failed\n");
        exit(127);
    }

    /* Make the shell the foreground job of the console while it runs, so
     * job control and Ctrl-C behave; init ignores SIGTTOU, so this
     * tcsetpgrp from a non-foreground process is safe (as in run_rc). */
    setpgid(pid, pid);
    tcsetpgrp(0, pid);

    int status = 0;
    waitpid(pid, &status, 0);
    print("init: single-user shell exited, continuing boot\n");
}

int main(int argc, char **argv)
{
    ignore_terminal_signals();

    sys_dbgputs("INITDBG: main entered (new init)");

    print("\nGNOS init: pid ");
    printn(getpid());
    print(" - starting the session\n");

    /* Kernel command line words that the kernel did not consume arrive here
     * as argv[1..], Linux style; "single" / "S" / "-s" mean single-user. */
    int single = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "single") == 0 ||
            strcmp(argv[i], "S") == 0 ||
            strcmp(argv[i], "-s") == 0)
            single = 1;
    }

    if (single)
        run_single_user();      /* drops to root bash; returns on exit */

    run_rc();                       /* one-shot /etc/rc before the prompts */

    /*
     * One getty per virtual terminal, respawned for ever -- the classic
     * inittab "respawn" action, with the table compiled in.  gettys[i] holds
     * the pid currently serving terminal i+1, or a negative number when that
     * terminal has been given up on.
     */
    int gettys[NR_TTY];
    int failed[NR_TTY];
    for (int i = 0; i < NR_TTY; i++) {
        failed[i] = 0;
        gettys[i] = spawn_getty(i + 1);
    }

    for (;;) {
        int status = 0;
        int who = waitpid(-1, &status, 0);

        if (who < 0) {
            /* Nothing left to wait for.  Every terminal must have been given
             * up on; there is no work left for init to do but stay alive. */
            int any = 0;
            for (int i = 0; i < NR_TTY; i++)
                if (gettys[i] > 0)
                    any = 1;
            if (!any) {
                print("init: no terminals left, halting supervision\n");
                for (;;)
                    ;
            }
            continue;
        }

        int slot = -1;
        for (int i = 0; i < NR_TTY; i++)
            if (gettys[i] == who)
                slot = i;

        if (slot < 0)                   /* an orphan re-parented onto us */
            continue;

        /* Exit code 127 is what our own child uses when execv() fails, so it
         * is the one case where respawning cannot possibly help. */
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            failed[slot]++;
        else
            failed[slot] = 0;

        if (failed[slot] > MAX_FAST_RESTARTS) {
            print("init: giving up on terminal ");
            printn(slot + 1);
            print("\n");
            gettys[slot] = -1;
            continue;
        }

        gettys[slot] = spawn_getty(slot + 1);
    }
}
