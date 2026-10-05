#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "lib/str/str.h"
#include "kernel/fs/fs.h"
#include "syscall.h"

static int g_fail = 0;

static void expect(int cond, const char *msg) {
    if (!cond) {
        g_fail = 1;
        printf("  [FAIL] %s\n", msg);
    }
}

int main(void) {
    printf("dev_demo: start\n");

    struct FS_STAT st;
    expect(stat("/dev/null", &st) == 0, "stat /dev/null");
    expect(st.st_filetype == FT_CHARDEVICE, "/dev/null is chardev");

    int fd = open("/dev/null", O_RDWR);
    expect(fd >= 0, "open /dev/null");
    if (fd >= 0) {
        char wbuf[64];
        memset(wbuf, 'x', sizeof(wbuf));
        expect(write(fd, wbuf, sizeof(wbuf)) == (int32_t)sizeof(wbuf),
               "write /dev/null swallows");
        char rbuf[16];
        expect(read(fd, rbuf, sizeof(rbuf)) == 0, "read /dev/null EOF");
        close(fd);
    }

    fd = open("/dev/zero", O_RDONLY);
    expect(fd >= 0, "open /dev/zero");
    if (fd >= 0) {
        char zbuf[32];
        memset(zbuf, 1, sizeof(zbuf));
        expect(read(fd, zbuf, sizeof(zbuf)) == (int32_t)sizeof(zbuf),
               "read /dev/zero full");
        int zeroed = 1;
        for (int i = 0; i < (int)sizeof(zbuf); i++) {
            if (zbuf[i] != 0) {
                zeroed = 0;
                break;
            }
        }
        expect(zeroed, "read /dev/zero zeroed");
        close(fd);
    }

    fd = open("/dev/console", O_WRONLY);
    expect(fd >= 0, "open /dev/console");
    if (fd >= 0) {
        expect(write(fd, "dev_demo: console write ok\n", 27) == 27,
               "write /dev/console");
        close(fd);
    }

    expect(mkdir("/dev") == -1, "mkdir /dev exists");

    if (g_fail == 0) {
        printf("dev_demo: PASS\n");
        exit(0);
    }
    printf("dev_demo: FAIL\n");
    exit(1);
}
