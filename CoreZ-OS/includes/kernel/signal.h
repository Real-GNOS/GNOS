#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>

struct TASK;
struct ARCH_REGS;

#define NSIG 32
#define EINTR 4

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
#define SIGWINCH 28
#define SIGSYS 31

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x0001
#define SA_NODEFER 0x0002
#define SA_RESTORER 0x04000000
#define SA_SIGINFO 0x0004

#define SIG_ACT_TERM 0
#define SIG_ACT_IGN 1
#define SIG_ACT_STOP 2
#define SIG_ACT_CONT 3

typedef uint32_t sigset_t;

struct SYS_SIGACTION {
    void (*sa_handler)(int);
    uint32_t sa_mask;
    uint32_t sa_flags;
    void (*sa_restorer)(void);
};

struct SYS_SIGFRAME {
    uint32_t restorer;
    uint32_t signo;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t user_esp;
    uint32_t ss;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t old_mask;
};

void init_signal_state(struct TASK *t);
void signal_reset_user(struct TASK *t);

int sys_sigaction(int sig, const struct SYS_SIGACTION *act, struct SYS_SIGACTION *old);
int sys_kill(int pid, int sig);
int sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
uint64_t sys_sigreturn(struct ARCH_REGS *r);

void check_pending_signals(struct ARCH_REGS *r);
void itimer_tick(void);

void signal_terminate(struct TASK *t, int sig);
void signal_notify_child_exit(struct TASK *parent);

int exception_to_signal(int int_no);

#endif
