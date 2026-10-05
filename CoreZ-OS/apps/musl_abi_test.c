#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <grp.h>
#include <time.h>

static char altbuf[8192];
static stack_t oldss;
static volatile int usr1_flag;
static volatile int alarm_flag;

static void usr1_handler(int sig) {
    (void)sig;
    usr1_flag = 1;
}

static void alrm_handler(int sig) {
    (void)sig;
    alarm_flag = 1;
}

int main(int argc, char **argv) {
    printf("[abi] start argc=%d argv0=%s argv1=%s\n", argc,
           argc > 0 ? argv[0] : "", argc > 1 ? argv[1] : "");
    if (argc > 1 && strcmp(argv[1], "envcheck") == 0) {
        printf("[abi] env FOO=%s\n", getenv("FOO"));
        return 66;
    }
    DIR *d;
    int nd;
    int pfd[2];
    int fd = open("/cat.elf", O_RDONLY);
    printf("[abi] openat fd=%d\n", fd);
    char buf[8];
    ssize_t n = read(fd, buf, 4);
    printf("[abi] read n=%d elf=%c%c\n", (int)n, buf[0], buf[1]);
    printf("[abi] lseek=%d\n", (int)lseek(fd, 0, SEEK_SET));
    struct stat st;
    int r = fstat(fd, &st);
    printf("[abi] fstat r=%d size=%d reg=%d\n", r, (int)st.st_size,
           S_ISREG(st.st_mode));
    r = stat("/cat.elf", &st);
    printf("[abi] stat r=%d size=%d reg=%d\n", r, (int)st.st_size,
           S_ISREG(st.st_mode));
    write(1, "[abi] S1 close\n", 15);
    close(fd);
    write(1, "[abi] S2 opendir\n", 17);
    int dd = open("/", O_RDONLY | O_DIRECTORY);
    printf("[abi] open / dir fd=%d\n", dd);
    if (dd >= 0)
        close(dd);
    d = opendir("/");
    write(1, "[abi] S3 readdir\n", 17);
    nd = 0;
    if (d != NULL) {
        while (readdir(d) != NULL) {
            nd++;
        }
        closedir(d);
    } else {
        write(1, "[abi] opendir NULL\n", 19);
    }
    printf("[abi] readdir n=%d\n", nd);
    write(1, "[abi] S4 pipe\n", 14);
    pfd[0] = -1;
    pfd[1] = -1;
    printf("[abi] pipe r=%d\n", pipe(pfd));
    write(pfd[1], "pipe-ok", 7);
    char pb[8];
    read(pfd[0], pb, 7);
    pb[7] = 0;
    printf("[abi] pipe data=%s\n", pb);
    close(pfd[0]);
    close(pfd[1]);
    int f1 = open("/cat.elf", O_RDONLY);
    int f2 = dup2(f1, 7);
    printf("[abi] dup2 r=%d\n", f2);
    close(f1);
    close(f2);
    char *cargv[] = {(char *)"musl_abi_test", (char *)"envcheck", NULL};
    char *cenv[] = {(char *)"FOO=bar42", NULL};
    pid_t pid = fork();
    printf("[abi] fork pid=%d\n", (int)pid);
    if (pid == 0) {
        execve("/musl_abi_test.elf", cargv, cenv);
        printf("[abi] execve errno=%d\n", errno);
        _exit(127);
    }
    int ws = 0;
    int wr = waitpid(pid, &ws, 0);
    printf("[abi] waitpid r=%d raw=0x%x status=%d\n", wr, (unsigned)ws,
           WEXITSTATUS(ws));
    int sl = symlink("/cat.elf", "/sltest");
    char lb[128];
    ssize_t lr = readlink("/sltest", lb, sizeof(lb) - 1);
    if (lr >= 0) lb[lr] = 0;
    printf("[abi] symlink r=%d readlink=\'%s\' n=%d\n", sl,
           lr >= 0 ? lb : "?", (int)lr);
    int lf = sl == 0 ? open("/sltest", O_RDONLY) : -1;
    char mb[4] = {0};
    int mr = lf >= 0 ? (int)read(lf, mb, 4) : -1;
    printf("[abi] link-open r=%d elf=%d\n", mr, mb[0] == 0x7f && mb[1] == 'E');
    if (lf >= 0) close(lf);
    int sl2 = symlink("/cat.elf" "/sub/dir/padding/xyz/sub/dir/padding/xyz/"
                      "sub/dir/padding/xyz",
                      "/slslow");
    char lb2[128];
    ssize_t lr2 = readlink("/slslow", lb2, sizeof(lb2) - 1);
    printf("[abi] slow-symlink r=%d readlink n=%d fast=\'%.*s\'\n", sl2,
           (int)lr2, lr2 > 8 ? 8 : (int)lr2, lb2);
    printf("[abi] link-unlink r=%d errno=%d\n", unlink("/sltest"), errno);
    printf("[abi] img-symlink fd=%d\n", open("/catlink", O_RDONLY));
    int mk = mknod("/dev/apitest", S_IFCHR | 0666, makedev(1, 5));
    printf("[abi] mknod r=%d errno=%d\n", mk, errno);
    int zf = mk == 0 ? open("/dev/apitest", O_RDONLY) : -1;
    char ab[4] = {1, 1, 1, 1};
    int rn = zf >= 0 ? (int)read(zf, ab, 4) : -1;
    printf("[abi] mknod-dev read r=%d zero=%d\n", rn,
           ab[0] == 0 && ab[3] == 0);
    if (zf >= 0) close(zf);
    printf("[abi] unlink r=%d errno=%d\n", unlink("/dev/apitest"), errno);
    int sp2 = fork();
    if (sp2 == 0) {
        int s2 = setsid();
        if (s2 < 0)
            _exit(2);
        _exit((int)getsid(0) == (int)getpid() ? 9 : 3);
    }
    int st1 = 0;
    waitpid(sp2, &st1, 0);
    printf("[abi] setsid-child exit=%d\n", WEXITSTATUS(st1));
    struct statfs sf;
    int sr = statfs("/", &sf);
    printf("[abi] statfs r=%d magic=%lx bsize=%lu blocks=%lu\n", sr,
           (unsigned long)sf.f_type, (unsigned long)sf.f_bsize,
           (unsigned long)sf.f_blocks);
    struct rusage ru;
    int gr = getrusage(RUSAGE_SELF, &ru);
    printf("[abi] getrusage r=%d utime=%ld.%06ld\n", gr,
           (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec);
    stack_t sa2 = {.ss_sp = altbuf, .ss_size = sizeof(altbuf), .ss_flags = 0};
    int sg = sigaltstack(&sa2, &oldss);
    printf("[abi] sigaltstack r=%d errno=%d oldflags=%d\n", sg, errno,
           oldss.ss_flags);
    struct sigaction us1 = {.sa_handler = usr1_handler};
    sigaction(SIGUSR1, &us1, 0);
    struct sigaction alm = {.sa_handler = alrm_handler};
    sigaction(SIGALRM, &alm, 0);
    sigset_t sm;
    sigemptyset(&sm);
    sigaddset(&sm, SIGUSR1);
    sigprocmask(SIG_BLOCK, &sm, 0);
    raise(SIGUSR1);
    int wm = fork();
    printf("[abi] waitid-fork pid=%d\n", wm);
    if (wm == 0)
        _exit(42);
    siginfo_t wi;
    memset(&wi, 0, sizeof(wi));
    int wr2 = waitid(P_PID, wm, &wi, WEXITED);
    printf("[abi] waitid r=%d pid=%d code=%d status=%d\n", wr2, wi.si_pid,
           wi.si_code, wi.si_status);
    struct itimerval it = {.it_value = {.tv_sec = 0, .tv_usec = 50000}};
    int st2 = setitimer(ITIMER_REAL, &it, 0);
    struct itimerval itg;
    getitimer(ITIMER_REAL, &itg);
    printf("[abi] setitimer r=%d getitimer=%ld us\n", st2,
           (long)(itg.it_value.tv_sec * 1000000 + itg.it_value.tv_usec));
    for (int q = 0; q < 40 && !alarm_flag; q++) {
        struct timespec ts50 = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&ts50, 0);
    }
    printf("[abi] alarm-fired=%d\n", alarm_flag);
    sigset_t empty;
    sigemptyset(&empty);
    int ss2 = sigsuspend(&empty);
    printf("[abi] sigsuspend r=%d errno=%d handler=%d\n", ss2, errno,
           usr1_flag);
    gid_t gb[16];
    int gn = 16;
    int gl = getgrouplist("user", 1000, gb, &gn);
    printf("[abi] getgrouplist r=%d n=%d errno=%d\n", gl, gn,
           gl < 0 ? errno : 0);
    int sgp = setgroups(1, gb);
    printf("[abi] setgroups r=%d errno=%d\n", sgp, sgp < 0 ? errno : 0);
    int su = setuid(1000);
    int shfd = open("/etc/shadow", O_RDONLY);
    int pwfd = open("/etc/passwd", O_RDONLY);
    printf("[abi] perm: setuid=%d shadow-errno=%d passwd-ok=%d uid=%d\n",
           su, shfd < 0 ? errno : 0, pwfd >= 0, (int)getuid());
    if (shfd >= 0) close(shfd);
    if (pwfd >= 0) close(pwfd);
    int sp3 = fork();
    if (sp3 == 0) {
        char *cargv2[] = {(char *)"sh", (char *)"-c", (char *)"id", NULL};
        execve("/suidsh", cargv2, cenv);
        printf("[abi] suid-exec errno=%d\n", errno);
        _exit(99);
    }
    int st3 = 0;
    waitpid(sp3, &st3, 0);
    printf("[abi] suid-test done status=%d\n", WEXITSTATUS(st3));
    printf("[abi] ALL PASS\n");
    return 0;
}
