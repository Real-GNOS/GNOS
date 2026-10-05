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
    printf("mmap_demo: start\n");

    char *p = (char *)mmap(0, 0x3000, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "mmap anon 3 pages");
    CHECK(((uint32_t)p & 0xfff) == 0, "mmap page aligned");

    int zeroed = 1;
    uint32_t *wp = (uint32_t *)p;
    for (int i = 0; i < 0x3000 / 4; i++) {
        if (wp[i] != 0) {
            zeroed = 0;
            break;
        }
    }
    CHECK(zeroed, "mmap region zeroed");

    for (int i = 0; i < 0x3000 / 4; i++) {
        wp[i] = (uint32_t)(i * 0x101 + 7);
    }
    int intact = 1;
    for (int i = 0; i < 0x3000 / 4; i++) {
        if (wp[i] != (uint32_t)(i * 0x101 + 7)) {
            intact = 0;
            break;
        }
    }
    CHECK(intact, "mmap write/read payload");

    char *q = (char *)mmap(0, 0x1000, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(q != MAP_FAILED, "mmap second region");
    CHECK(p != q, "two mmap regions distinct");
    intact = 1;
    for (int i = 0; i < 0x3000 / 4; i++) {
        if (wp[i] != (uint32_t)(i * 0x101 + 7)) {
            intact = 0;
            break;
        }
    }
    CHECK(intact, "first region intact after second mmap");

    CHECK(munmap(q, 0x1000) == 0, "munmap second region");

    CHECK(mprotect(p, 0x3000, PROT_READ) == 0, "mprotect read-only");
    CHECK(wp[0] == 7, "read still ok after mprotect");

    CHECK(mmap(0, 0, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) ==
              MAP_FAILED,
          "mmap len 0 rejects");

    CHECK(munmap(p, 0x3000) == 0, "munmap first region");

    if (g_fail == 0) {
        printf("mmap_demo: PASS\n");
        exit(0);
    }
    printf("mmap_demo: FAIL\n");
    exit(1);
    return 0;
}
