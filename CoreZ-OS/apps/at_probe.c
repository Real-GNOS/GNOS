#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, passed;
#define CK(c)                                                                                                              \
    do {                                                                                                                   \
        checks++;                                                                                                          \
        if (c)                                                                                                             \
            passed++;                                                                                                      \
        else                                                                                                               \
            printf("  at_probe fail@%d\n", __LINE__);                                                                       \
    } while (0)

int main(void) {
    struct stat st;
    char cwd[128];
    char buf[16];
    int fd = openat(AT_FDCWD, "/lib", O_RDONLY | O_DIRECTORY);

    CK(fd >= 0);
    CK(fstatat(fd, "libc.so", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
       S_ISREG(st.st_mode) && st.st_size > 0);
    CK(fstatat(fd, "../etc/passwd", &st, 0) == 0 && S_ISREG(st.st_mode));
    int f2 = openat(fd, "libc.so", O_RDONLY);
    CK(f2 >= 0 && read(f2, buf, 4) == 4 && memcmp(buf, "\177ELF", 4) == 0);
    if (f2 >= 0)
        close(f2);
    if (fd >= 0)
        close(fd);

    CK(lstat("/bin/sh", &st) == 0 && S_ISLNK(st.st_mode));
    CK(stat("/bin/sh", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);

    int tfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CK(tfd >= 0);
    int nf = openat(tfd, "atp", O_CREAT | O_RDWR, 0644);
    CK(nf >= 0);
    if (nf >= 0)
        close(nf);
    CK(fstatat(tfd, "atp", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
       S_ISREG(st.st_mode));
    CK(chdir("/tmp") == 0);
    CK(getcwd(cwd, sizeof cwd) != 0 && strcmp(cwd, "/tmp") == 0);
    CK(stat("atp", &st) == 0 && S_ISREG(st.st_mode));
    CK(unlink("atp") == 0);

    int mfd = open("/tmp/atm", O_CREAT | O_RDWR, 0644);
    CK(mfd >= 0);
    if (mfd >= 0)
        close(mfd);
    CK(stat("/tmp/atm", &st) == 0 && (st.st_mode & 07777u) == 0644u &&
       st.st_mtime > 0);
    struct timespec ts2[2] = {{1000000000, 0}, {1000000000, 0}};
    CK(utimensat(AT_FDCWD, "/tmp/atm", ts2, 0) == 0);
    CK(stat("/tmp/atm", &st) == 0 && st.st_mtim.tv_sec == 1000000000);
    int wfd = open("/tmp/atm", O_WRONLY);
    CK(wfd >= 0 && write(wfd, "x", 1) == 1);
    if (wfd >= 0)
        close(wfd);
    CK(stat("/tmp/atm", &st) == 0 && st.st_mtime > 1000000000);
    CK(unlink("/tmp/atm") == 0);
    CK(stat("/lib", &st) == 0 && S_ISDIR(st.st_mode) && st.st_nlink >= 2);

    chdir("/");
    if (tfd >= 0)
        close(tfd);

    CK(chdir("/etc") == 0);
    CK(getcwd(cwd, sizeof cwd) != 0 && strcmp(cwd, "/etc") == 0);
    CK(stat("passwd", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    int f3 = open("shadow", O_RDONLY);
    CK(f3 >= 0 && read(f3, buf, 8) == 8);
    if (f3 >= 0)
        close(f3);
    int f4 = openat(AT_FDCWD, "passwd", O_RDONLY);
    CK(f4 >= 0);
    if (f4 >= 0)
        close(f4);

    printf("at_probe: %s %d/%d\n", passed == checks ? "PASS" : "FAIL", passed,
           checks);
    return passed == checks ? 0 : 1;
}
