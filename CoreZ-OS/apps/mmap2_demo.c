#include "libc/user/stdio.h"
#include "syscall.h"

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
    printf("mmap2_demo: start\n");

    char *p = (char *)mmap2(0, 0x3000, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 2);
    CHECK(p != MAP_FAILED, "mmap2 anon 3 pages");
    CHECK(((uint32_t)p & 0xfff) == 0, "mmap2 page aligned");

    int zeroed = 1;
    uint32_t *wp = (uint32_t *)p;
    for (int i = 0; i < 0x3000 / 4; i++) {
        if (wp[i] != 0) {
            zeroed = 0;
            break;
        }
    }
    CHECK(zeroed, "mmap2 region zeroed");

    for (int i = 0; i < 0x3000 / 4; i++) {
        wp[i] = (uint32_t)(i * 0x13 + 5);
    }
    int intact = 1;
    for (int i = 0; i < 0x3000 / 4; i++) {
        if (wp[i] != (uint32_t)(i * 0x13 + 5)) {
            intact = 0;
            break;
        }
    }
    CHECK(intact, "mmap2 write/read payload");

    CHECK(mprotect(p, 0x3000, PROT_READ) == 0, "mprotect read-only");
    CHECK(wp[0] == 5, "read after mprotect");

    char *q = (char *)mmap2(0, 0x1000, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(q != MAP_FAILED, "mmap2 second region");
    CHECK(p != q, "two mmap2 regions distinct");

    CHECK(munmap(q, 0x1000) == 0, "munmap second region");
    CHECK(wp[0] == 5, "first region intact after munmap");

    CHECK(mmap2(0, 0, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) ==
              MAP_FAILED,
          "mmap2 len 0 rejects");

    void *h =
        mmap2((void *)1, 0x1000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(h != MAP_FAILED && ((uint32_t)h & 0xfff) == 0,
          "mmap2 unaligned hint ignored");
    if (h != MAP_FAILED) {
        munmap(h, 0x1000);
    }

    CHECK(munmap(p, 0x3000) == 0, "munmap first region");

    if (g_fail == 0) {
        printf("mmap2_demo: PASS\n");
        exit(0);
    }
    printf("mmap2_demo: FAIL\n");
    exit(1);
    return 0;
}
