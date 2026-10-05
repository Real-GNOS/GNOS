#include "libc/user/stdio.h"
#include "syscall.h"

enum FS_FILE_TYPE { FT_UNKNOWN, FT_REGULAR, FT_DIRECTORY };
struct FS_STAT {
    uint32_t st_ino;
    uint32_t st_size;
    enum FS_FILE_TYPE st_filetype;
};

static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_fail = 1;                                                        \
            printf("  [FAIL] %s\n", msg);                                      \
        }                                                                      \
    } while (0)

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *f1 = "/sysdemo.tmp";
    const char *f2 = "/sysdemo.renamed";
    struct FS_STAT st;

    printf("fsyscall_demo: start\n");

    CHECK(getuid() == 0, "getuid==0");
    CHECK(getgid() == 0, "getgid==0");
    CHECK(geteuid() == 0, "geteuid==0");
    CHECK(getegid() == 0, "getegid==0");

    CHECK(access("/definitely_not_exist", 0) == -1, "access missing -> -1");
    int fd = open(f1, 4);
    CHECK(fd >= 0, "open O_CREAT");
    CHECK(access(f1, 0) == 0, "access exists -> 0");

    CHECK(fstat(fd, &st) == 0, "fstat ok");
    CHECK(st.st_filetype == FT_REGULAR, "fstat type regular");
    CHECK(stat(f1, &st) == 0 && st.st_filetype == FT_REGULAR, "stat ok");

    int d = dup(fd);
    CHECK(d >= 0 && d != fd, "dup new fd");
    CHECK(d >= 0 && fstat(d, &st) == 0, "fstat on dup fd");
    int d2 = dup2(fd, 6);
    CHECK(d2 == 6, "dup2 to target");
    CHECK(fcntl(fd, F_GETFL, 0) >= 0, "fcntl F_GETFL");
    CHECK(fcntl(fd, F_SETFL, 4) == 0, "fcntl F_SETFL");
    int d3 = fcntl(fd, F_DUPFD, 0);
    CHECK(d3 >= 0 && d3 != fd, "fcntl F_DUPFD");
    CHECK(fcntl(fd, F_GETFD, 0) == 0, "fcntl F_GETFD");
    CHECK(fcntl((-1), F_GETFL, 0) == -1, "fcntl bad fd -> -1");

    char dbuf[256];
    CHECK(getdents(-1, (struct LINUX_DIRENT *)dbuf, sizeof(dbuf)) == -1,
          "getdents bad fd -> -1");

    CHECK(chmod(f1, 0700) == 0, "chmod ok");
    CHECK(truncate(f1, 5) == 0, "truncate ok");
    CHECK(rename(f1, f2) == 0, "rename ok");
    CHECK(access(f1, 0) == -1, "old name gone");
    CHECK(access(f2, 0) == 0, "new name present");
    CHECK(chmod(f2, 0644) == 0, "chmod renamed");

    CHECK(readlink(f2, dbuf, sizeof(dbuf)) == -1, "readlink -> -1");

    struct SYS_TIMESPEC ts;
    struct SYS_TIMEVAL tv;
    CHECK(clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec >= 0,
          "clock_gettime");
    CHECK(gettimeofday(&tv, 0) == 0 && tv.tv_sec >= 0, "gettimeofday");
    struct SYS_TIMESPEC req = {0, 10 * 1000 * 1000};
    CHECK(nanosleep(&req, 0) == 0, "nanosleep 10ms");

    close(fd);
    close(d);
    close(d2);
    close(d3);
    CHECK(unlink(f2) == 0, "cleanup unlink");

    if (g_fail == 0) {
        printf("fsyscall_demo: PASS\n");
        exit_group(0);
    }
    printf("fsyscall_demo: FAIL\n");
    exit_group(1);
    return 0;
}
