#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

#include <stdint.h>

typedef int32_t pid_t;

#define NSIG 32

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGSYS 31

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int)) - 1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x0001
#define SA_NODEFER 0x0002
#define SA_RESTORER 0x04000000
#define SA_SIGINFO 0x0004

typedef uint32_t sigset_t;

struct SYS_SIGACTION {
    void (*sa_handler)(int);
    uint32_t sa_mask;
    uint32_t sa_flags;
    void (*sa_restorer)(void);
};

void __restore(void);

int sigaction(int sig, const struct SYS_SIGACTION *act, struct SYS_SIGACTION *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int kill(pid_t pid, int sig);
void (*signal(int sig, void (*handler)(int)))(int);

#endif
