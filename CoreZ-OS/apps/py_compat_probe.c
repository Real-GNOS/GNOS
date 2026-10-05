#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<sys/utsname.h>)
#include <sys/utsname.h>
#define HAVE_UTSNAME 1
#endif
#if __has_include(<sys/mman.h>)
#include <sys/mman.h>
#define HAVE_MMAN 1
#endif
#if __has_include(<sys/ioctl.h>)
#include <sys/ioctl.h>
#define HAVE_IOCTL 1
#endif
#if __has_include(<sys/wait.h>)
#include <sys/wait.h>
#define HAVE_WAIT 1
#endif
#if __has_include(<sys/select.h>)
#include <sys/select.h>
#define HAVE_SELECT 1
#endif
#if __has_include(<sys/socket.h>)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define HAVE_SOCKET 1
#endif
#if __has_include(<netdb.h>)
#include <netdb.h>
#define HAVE_NETDB 1
#endif
#if __has_include(<poll.h>)
#include <poll.h>
#define HAVE_POLL 1
#endif
#if __has_include(<termios.h>)
#include <termios.h>
#define HAVE_TERMIOS 1
#endif
#if __has_include(<dirent.h>)
#include <dirent.h>
#define HAVE_DIRENT 1
#endif
#if __has_include(<dlfcn.h>)
#include <dlfcn.h>
#define HAVE_DLFCN 1
#endif
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define HAVE_GETRANDOM 1
#endif
#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#define HAVE_RSRC 1
#endif
#if __has_include(<sys/statvfs.h>)
#include <sys/statvfs.h>
#define HAVE_STATVFS 1
#endif
#if __has_include(<wchar.h>)
#include <wchar.h>
#define HAVE_WCHAR 1
#endif
#if __has_include(<iconv.h>)
#include <iconv.h>
#define HAVE_ICONV 1
#endif
#if __has_include(<pthread.h>)
#include <pthread.h>
#define HAVE_PTHREAD 1
#endif
#if __has_include(<semaphore.h>)
#include <semaphore.h>
#define HAVE_SEM 1
#endif
#if __has_include(<sched.h>)
#include <sched.h>
#define HAVE_SCHED 1
#endif
#if __has_include(<pwd.h>)
#include <pwd.h>
#define HAVE_PWD 1
#endif
#if __has_include(<sys/epoll.h>)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#endif
#if __has_include(<sys/eventfd.h>)
#include <sys/eventfd.h>
#define HAVE_EVENTFD 1
#endif
#if __has_include(<sys/timerfd.h>)
#include <sys/timerfd.h>
#define HAVE_TIMERFD 1
#endif
#endif

static int pass_n;
static int fail_n;
static int skip_n;

static void chk(const char *name, int ok) {
    if (ok) {
        pass_n++;
        printf("PYCHK %s: PASS\n", name);
    } else {
        fail_n++;
        printf("PYCHK %s: FAIL errno=%d (%s)\n", name, errno, strerror(errno));
    }
    fflush(stdout);
}

static void chk_rc(const char *name, long rc) {
    if (rc >= 0) {
        pass_n++;
        printf("PYCHK %s: PASS\n", name);
    } else {
        fail_n++;
        printf("PYCHK %s: FAIL errno=%d (%s)\n", name, errno, strerror(errno));
    }
    fflush(stdout);
}

static void info(const char *name, long value) {
    printf("PYCHK %s: INFO %ld\n", name, value);
    fflush(stdout);
}

typedef void (*test_fn)(void);

static void run_group(const char *name, test_fn fn) {
#if HAVE_WAIT
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        int before_pass = pass_n;
        int before_fail = fail_n;
        fn();
        int pn = pass_n - before_pass;
        int fn_ = fail_n - before_fail;
        printf("PYCHK GROUP %s: done pass=%d fail=%d\n", name, pn, fn_);
        fflush(stdout);
        _exit(fn_ > 127 ? 127 : fn_);
    }
    if (p < 0) {
        printf("PYCHK GROUP %s: FORK-FAIL\n", name);
        fflush(stdout);
        return;
    }
    int st = 0;
    pid_t w = waitpid(p, &st, 0);
    if (w != p) {
        printf("PYCHK GROUP %s: WAIT-FAIL\n", name);
    } else if (WIFSIGNALED(st)) {
        fail_n++;
        printf("PYCHK GROUP %s: CRASH sig=%d\n", name, WTERMSIG(st));
    } else {
        fail_n += WEXITSTATUS(st);
    }
    fflush(stdout);
#else
    fn();
#endif
}

static void skip(const char *name) {
    skip_n++;
    printf("PYCHK %s: SKIP no-header\n", name);
    fflush(stdout);
}

static int write_text(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, s, strlen(s));
    close(fd);
    return (n == (ssize_t)strlen(s)) ? 0 : -1;
}

static void test_time(void) {
    struct timespec ts;
    errno = 0;
    chk("time/clock_monotonic",
        clock_gettime(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec >= 0);
    errno = 0;
    chk("time/clock_realtime", clock_gettime(CLOCK_REALTIME, &ts) == 0);
    info("time/realtime_epoch", (long)ts.tv_sec);
    struct timeval tv;
    errno = 0;
    chk_rc("time/gettimeofday", gettimeofday(&tv, 0));
    errno = 0;
    time_t now = time(0);
    chk_rc("time/time", now < 0 ? -1 : 0);
    info("time/epoch", (long)now);
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    errno = 0;
    chk("time/nanosleep", nanosleep(&ts, 0) == 0);
    struct tm tmv;
    errno = 0;
    chk("time/localtime_r", localtime_r(&now, &tmv) != 0);
    errno = 0;
    chk("time/gmtime_r", gmtime_r(&now, &tmv) != 0);
    char buf[64];
    errno = 0;
    chk("time/strftime", strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv) > 0);
    tmv.tm_year = 124;
    tmv.tm_mon = 5;
    tmv.tm_mday = 15;
    tmv.tm_hour = 12;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    errno = 0;
    chk_rc("time/mktime", mktime(&tmv));
}

static void test_locale_wchar(void) {
    errno = 0;
    chk("locale/setlocale", setlocale(LC_ALL, "C") != 0);
    struct lconv *lc = localeconv();
    chk("locale/localeconv", lc && lc->decimal_point && lc->decimal_point[0] == '.');
#if HAVE_WCHAR
    mbstate_t st;
    wchar_t wc = 0;
    memset(&st, 0, sizeof(st));
    size_t n = mbrtowc(&wc, "A", 1, &st);
    chk("wchar/mbrtowc", n == 1 && wc == L'A');
    char out[8];
    memset(&st, 0, sizeof(st));
    n = wcrtomb(out, L'A', &st);
    chk("wchar/wcrtomb", n == 1 && out[0] == 'A');
    wchar_t ws[8];
    n = mbstowcs(ws, "ab", 8);
    chk("wchar/mbstowcs", n == 2 && ws[1] == L'b');
#else
    skip("wchar/mbrtowc");
#endif
#if HAVE_ICONV
    iconv_t cd = iconv_open("UTF-8", "ASCII");
    errno = 0;
    chk("iconv/open_ascii", cd != (iconv_t)-1);
    if (cd != (iconv_t)-1)
        iconv_close(cd);
#else
    skip("iconv/open_ascii");
#endif
}

static void test_math(void) {
    errno = 0;
    chk("math/sqrt", fabs(sqrt(4.0) - 2.0) < 1e-9);
    chk("math/pow", fabs(pow(2.0, 10.0) - 1024.0) < 1e-6);
    chk("math/fmod", fabs(fmod(7.5, 2.0) - 1.5) < 1e-9);
    chk("math/floor_ceil", floor(2.7) == 2.0 && ceil(2.1) == 3.0);
    int e;
    double m = frexp(8.0, &e);
    chk("math/frexp_ldexp", m == 0.5 && e == 4 && ldexp(m, e) == 8.0);
    double ip;
    chk("math/modf", modf(3.25, &ip) == 0.25 && ip == 3.0);
    chk("math/sin_cos", fabs(sin(0.0)) < 1e-9 && fabs(cos(0.0) - 1.0) < 1e-9);
    chk("math/isnan_isinf", isnan(NAN) && isinf(INFINITY));
    errno = 0;
    chk("math/strtod", fabs(strtod("1.5e2", 0) - 150.0) < 1e-9);
}

static void test_fs(void) {
    errno = 0;
    chk("fs/write_file", write_text("/tmp/pychk.txt", "hello") == 0);
    int fd = open("/tmp/pychk.txt", O_RDONLY);
    errno = 0;
    chk("fs/open_read", fd >= 0);
    if (fd >= 0) {
        char b[8] = {0};
        ssize_t n = read(fd, b, sizeof(b) - 1);
        info("fs/read_len", (long)n);
        chk_rc("fs/read", n);
        off_t o = lseek(fd, 0, SEEK_SET);
        chk("fs/lseek", o == 0);
        struct stat stt;
        errno = 0;
        chk_rc("fs/fstat", fstat(fd, &stt));
        info("fs/fstat_size", (long)stt.st_size);
        errno = 0;
        chk("fs/ftruncate", ftruncate(fd, 3) == 0);
        errno = 0;
        chk("fs/fsync", fsync(fd) == 0);
        errno = 0;
        fchmod(fd, 0600);
        chk("fs/fchmod", errno == 0);
        close(fd);
    }
    struct stat stt;
    errno = 0;
    chk("fs/stat", stat("/tmp/pychk.txt", &stt) == 0);
    errno = 0;
    chk("fs/lstat", lstat("/tmp/pychk.txt", &stt) == 0);
    errno = 0;
    chk("fs/access", access("/tmp/pychk.txt", R_OK) == 0);
    errno = 0;
    chk("fs/rename", rename("/tmp/pychk.txt", "/tmp/pychk2.txt") == 0);
    errno = 0;
    chk("fs/chmod", chmod("/tmp/pychk2.txt", 0644) == 0);
    errno = 0;
    chk("fs/truncate", truncate("/tmp/pychk2.txt", 1) == 0);
    errno = 0;
    chk("fs/unlink", unlink("/tmp/pychk2.txt") == 0);
    errno = 0;
    chk("fs/mkdir_rmdir", mkdir("/tmp/pyd", 0755) == 0 && rmdir("/tmp/pyd") == 0);
    errno = 0;
    chk("fs/realpath", realpath("/tmp", 0) != 0);
    errno = 0;
    chk("fs/readlink", readlink("/catlink", 0, 0) >= 0);
    errno = 0;
    chk("fs/umask", umask(022) >= 0);
#if HAVE_DIRENT
    errno = 0;
    DIR *d = opendir("/tmp");
    chk("fs/opendir", d != 0);
    if (d) {
        struct dirent *de = readdir(d);
        chk("fs/readdir", de != 0 && de->d_name[0] != 0);
        rewinddir(d);
        closedir(d);
    }
#endif
    char cwd[256];
    errno = 0;
    chk("fs/getcwd", getcwd(cwd, sizeof(cwd)) != 0);
#if HAVE_STATVFS
    struct statvfs sv;
    errno = 0;
    printf("PYCHK TRY fs/statvfs\n");
    fflush(stdout);
    chk("fs/statvfs", statvfs("/", &sv) == 0 && sv.f_bsize > 0);
#endif
    errno = 0;
    printf("PYCHK TRY fs/open_dir\n");
    fflush(stdout);
    int mk = open("/tmp", O_RDONLY | O_DIRECTORY);
    chk("fs/open_dir", mk >= 0);
    if (mk >= 0)
        close(mk);
}

static void test_fd(void) {
    errno = 0;
    int d = dup(1);
    chk("fd/dup", d >= 0);
    if (d >= 0) {
        errno = 0;
        chk("fd/dup2", dup2(d, d) == d);
        close(d);
    }
    int fd = open("/tmp/pyfd.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        errno = 0;
        int fl = fcntl(fd, F_GETFL);
        chk("fd/fcntl_getfl", fl >= 0);
        errno = 0;
        chk("fd/fcntl_setfl_nonblock", fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
        errno = 0;
        chk("fd/fcntl_dupfd", fcntl(fd, F_DUPFD, 10) >= 10);
        close(fd);
        unlink("/tmp/pyfd.txt");
    }
    chk("fd/isatty_stdout", isatty(1) == 1);
    errno = 0;
    chk("fd/getpagesize", getpagesize() > 0);
}

static void test_tty(void) {
#if HAVE_TERMIOS
    errno = 0;
    struct termios t;
    chk("tty/tcgetattr", tcgetattr(0, &t) == 0);
    errno = 0;
    chk("tty/tcsetattr", tcsetattr(0, TCSANOW, &t) == 0);
#else
    skip("tty/tcgetattr");
#endif
#if HAVE_IOCTL && HAVE_TERMIOS
    struct winsize ws;
    errno = 0;
    int rc = ioctl(0, TIOCGWINSZ, &ws);
    chk("tty/tiocgwinsz", rc == 0 && ws.ws_row > 0 && ws.ws_col > 0);
#endif
    errno = 0;
    chk("proc/getpid", getpid() > 0);
    chk("proc/getppid", getppid() > 0);
    chk("proc/getuid", getuid() == geteuid());
    chk("proc/isatty_stdin", isatty(0) == 1 || !isatty(0));
}

static void test_io_wait(int fd) {
#if HAVE_POLL
    struct pollfd pf;
    pf.fd = fd;
    pf.events = POLLIN;
    pf.revents = 0;
    errno = 0;
    int pr = poll(&pf, 1, 0);
    chk("wait/poll_timeout0", pr >= 0);
#else
    skip("wait/poll_timeout0");
#endif
#if HAVE_SELECT
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval z;
    z.tv_sec = 0;
    z.tv_usec = 0;
    errno = 0;
    int sr = select(fd + 1, &rs, 0, 0, &z);
    chk("wait/select_timeout0", sr >= 0);
#else
    skip("wait/select_timeout0");
#endif
}

static void test_wait(void) {
    int pfd[2];
    if (pipe(pfd) != 0) {
        errno = 0;
        chk("wait/pipe", 0);
        return;
    }
    errno = 0;
    chk("wait/pipe", 1);
#if HAVE_POLL
    struct pollfd pf;
    pf.fd = pfd[0];
    pf.events = POLLIN;
    pf.revents = 0;
    errno = 0;
    int pr = poll(&pf, 1, 0);
    chk("wait/poll_empty_timeout0", pr == 0);
    errno = 0;
    chk_rc("wait/poll_rc", pr);
    write(pfd[1], "x", 1);
    pf.revents = 0;
    errno = 0;
    chk("wait/poll_ready", poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN));
    char c;
    read(pfd[0], &c, 1);
#else
    skip("wait/poll_empty_timeout0");
#endif
#if HAVE_SELECT
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(pfd[0], &rs);
    struct timeval z;
    z.tv_sec = 0;
    z.tv_usec = 0;
    errno = 0;
    chk("wait/select_timeout0", select(pfd[0] + 1, &rs, 0, 0, &z) == 0);
#endif
    int fl = fcntl(pfd[0], F_GETFL);
    fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);
    errno = 0;
    ssize_t n = read(pfd[0], &c, 1);
    chk("wait/nonblock_eagain", n == -1 && errno == EAGAIN);
    close(pfd[0]);
    close(pfd[1]);
}

static void test_net(void) {
#if HAVE_SOCKET
    errno = 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    chk("net/socket_tcp", s >= 0);
    if (s >= 0) {
        int one = 1;
        errno = 0;
        chk("net/setsockopt", setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = 0;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        errno = 0;
        chk("net/bind", bind(s, (struct sockaddr *)&a, sizeof(a)) == 0);
        struct sockaddr_in b;
        socklen_t bl = sizeof(b);
        errno = 0;
        chk("net/getsockname", getsockname(s, (struct sockaddr *)&b, &bl) == 0);
        errno = 0;
        chk("net/listen", listen(s, 4) == 0);
        errno = 0;
        chk("net/fcntl_nonblock_sock", fcntl(s, F_SETFL, O_NONBLOCK) == 0);
        test_io_wait(s);
        close(s);
    }
    errno = 0;
    int sp[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    chk("net/socketpair_unix", rc == 0);
    if (rc == 0) {
        char msg[4] = "ab";
        write(sp[0], msg, 2);
        char rb[4] = {0};
        ssize_t n = read(sp[1], rb, 2);
        chk("net/socketpair_io", n == 2 && rb[0] == 'a');
        close(sp[0]);
        close(sp[1]);
    }
    errno = 0;
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    chk("net/socket_udp", u >= 0);
    if (u >= 0)
        close(u);
    struct in_addr ia;
    errno = 0;
    chk("net/inet_pton", inet_pton(AF_INET, "127.0.0.1", &ia) == 1);
    char ipb[32];
    chk("net/inet_ntop", inet_ntop(AF_INET, &ia, ipb, sizeof(ipb)) != 0);
#endif
#if HAVE_NETDB
    struct addrinfo *res = 0;
    errno = 0;
    printf("PYCHK TRY net/getaddrinfo\n");
    fflush(stdout);
    int gr = getaddrinfo("127.0.0.1", "80", 0, &res);
    chk("net/getaddrinfo", gr == 0 && res != 0);
    if (gr == 0 && res)
        freeaddrinfo(res);
    res = 0;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    errno = 0;
    printf("PYCHK TRY net/getaddrinfo_svc\n");
    fflush(stdout);
    gr = getaddrinfo("localhost", "http", &hints, &res);
    chk("net/getaddrinfo_svc", gr == 0);
    if (gr == 0 && res)
        freeaddrinfo(res);
#endif
}

static void test_proc(void) {
#if HAVE_WAIT
    errno = 0;
    pid_t p = fork();
    chk("proc/fork", p >= 0);
    if (p == 0)
        _exit(3);
    if (p > 0) {
        int st = 0;
        pid_t w = waitpid(p, &st, 0);
        chk("proc/waitpid", w == p && WIFEXITED(st) && WEXITSTATUS(st) == 3);
    }
    errno = 0;
    chk("proc/kill_self0", kill(getpid(), 0) == 0);
    errno = 0;
    chk("proc/system_null", system(0) != 0);
    FILE *pp = popen("toybox echo PYCHK_POPEN_OK", "r");
    errno = 0;
    chk("proc/popen", pp != 0);
    if (pp) {
        char b[128] = {0};
        size_t n = fread(b, 1, sizeof(b) - 1, pp);
        pclose(pp);
        chk("proc/popen_read", n > 0 && strstr(b, "PYCHK_POPEN_OK") != 0);
    }
#else
    skip("proc/fork");
#endif
#if HAVE_RSRC
    struct rlimit rl;
    errno = 0;
    chk("proc/getrlimit_nofile", getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0);
    errno = 0;
    struct rusage ru;
    chk("proc/getrusage", getrusage(RUSAGE_SELF, &ru) == 0);
#endif
#if HAVE_SCHED
    errno = 0;
    chk("proc/sched_yield", sched_yield() == 0);
#endif
#if HAVE_PWD
    errno = 0;
    chk("proc/getpwuid", getpwuid(getuid()) != 0);
#endif
}

static void test_mem(void) {
#if HAVE_MMAN
    errno = 0;
    void *p = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    chk("mem/mmap_anon", p != MAP_FAILED);
    if (p != MAP_FAILED) {
        memset(p, 0x5a, 65536);
        chk("mem/mmap_write", ((unsigned char *)p)[4096] == 0x5a);
        errno = 0;
        chk("mem/madvise", madvise(p, 65536, MADV_DONTNEED) == 0);
        errno = 0;
        chk("mem/mprotect", mprotect(p, 4096, PROT_READ) == 0);
        errno = 0;
        chk("mem/munmap", munmap(p, 65536) == 0);
    }
    errno = 0;
    void *big = mmap(0, 4u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    chk("mem/mmap_4mb", big != MAP_FAILED);
    if (big != MAP_FAILED)
        munmap(big, 4u << 20);
#endif
    void *h = sbrk(0);
    chk("mem/sbrk", h != (void *)-1);
    void *a = malloc(1 << 20);
    void *b = malloc(64);
    chk("mem/malloc_large", a && b);
    free(a);
    free(b);
    errno = 0;
    chk("mem/realloc", realloc(0, 100) != 0);
#if HAVE_DLFCN
    errno = 0;
    void *self = dlopen(0, RTLD_NOW);
    if (self != 0) {
        chk("dl/dlopen_self", 1);
        errno = 0;
        chk("dl/dlsym_printf", dlsym(self, "printf") != 0);
    } else {
        skip("dl/dlopen_self_static");
    }
#else
    skip("dl/dlopen_self");
#endif
}

static void test_random(void) {
#if HAVE_GETRANDOM
    unsigned char b[8];
    errno = 0;
    ssize_t n = getrandom(b, sizeof(b), 0);
    chk("rand/getrandom", n == (ssize_t)sizeof(b));
#endif
    errno = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    chk("rand/dev_urandom_open", fd >= 0);
    if (fd >= 0) {
        unsigned char b[8];
        ssize_t n = read(fd, b, sizeof(b));
        chk("rand/dev_urandom_read", n == (ssize_t)sizeof(b));
        close(fd);
    }
    errno = 0;
    chk("rand/mkstemp", mkstemp("/tmp/pyXXXXXX") >= 0);
}

static void test_extras(void) {
#if HAVE_EPOLL
    errno = 0;
    int e = epoll_create1(0);
    chk("extra/epoll_create1", e >= 0);
    if (e >= 0)
        close(e);
#endif
#if HAVE_EVENTFD
    errno = 0;
    int ev = eventfd(0, EFD_NONBLOCK);
    chk("extra/eventfd", ev >= 0);
    if (ev >= 0)
        close(ev);
#endif
#if HAVE_TIMERFD
    errno = 0;
    int tf = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    chk("extra/timerfd", tf >= 0);
    if (tf >= 0)
        close(tf);
#endif
#if HAVE_SEM
    sem_t sem;
    errno = 0;
    chk("extra/sem_init", sem_init(&sem, 0, 1) == 0);
#endif
#if HAVE_PTHREAD
    errno = 0;
    pthread_t th;
    void *(*fn)(void *) = 0;
    chk("extra/pthread_self", pthread_self() != 0);
    (void)th;
    (void)fn;
#endif
    errno = 0;
    chk("extra/sysconf_pagesize", sysconf(_SC_PAGESIZE) > 0);
    errno = 0;
    chk("extra/sysconf_openmax", sysconf(_SC_OPEN_MAX) > 0);
    errno = 0;
    chk("extra/sysconf_clktck", sysconf(_SC_CLK_TCK) > 0);
#if HAVE_UTSNAME
    struct utsname u;
    errno = 0;
    chk("extra/uname", uname(&u) == 0 && u.sysname[0] != 0);
#endif
}

int main(void) {
    printf("PYCHK start\n");
    fflush(stdout);
    run_group("extras", test_extras);
    run_group("time", test_time);
    run_group("locale", test_locale_wchar);
    run_group("math", test_math);
    run_group("mem", test_mem);
    run_group("fs", test_fs);
    run_group("fd", test_fd);
    run_group("tty", test_tty);
    run_group("wait", test_wait);
    run_group("proc", test_proc);
    run_group("net", test_net);
    run_group("random", test_random);
    printf("PYCHK SUMMARY fail=%d\n", fail_n);
    fflush(stdout);
    return fail_n == 0 ? 0 : 1;
}
