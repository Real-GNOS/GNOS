/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#define SYS_preadv2       327
#define SYS_pwritev2      328
#define SYS_mount_setattr 442
#define SYS_statx         332
#define SYS_GNOS_BASE     1000
#define SYS_dbgputs       (SYS_GNOS_BASE + 1)
#define SYS_klog          (SYS_GNOS_BASE + 2)
#define SYS_process_madvise 440
#define SYS_pidfd_open    434

#ifndef AT_STATX_SYNC_AS_STAT
#define AT_STATX_SYNC_AS_STAT 0x0000
#endif

/* struct statx (x86-64, 256 bytes) */
struct statx {
    uint32_t mask, blksize; uint64_t attributes;
    uint32_t nlink, uid, gid;            /* u32, not u64 -- see struct statx */
    uint16_t mode, __p0; uint64_t ino, size, blocks; uint64_t attr_mask;
    struct { int64_t sec; uint32_t nsec, pad; } atime, btime, ctime, mtime;
    uint32_t rdev_maj, rdev_min, dev_maj, dev_min;
    uint64_t __pad1[11], mnt_id;
    uint32_t dio_mem_align, dio_offset_align;
    uint64_t subvol;
};

static int tests, fails;
#define CHECK(c, n) do { tests++; if (c) printf("ok  %s\n", n); \
    else { fails++; printf("FAIL %s\n", n); } } while (0)

struct iov { void *base; size_t len; };

int main(void)
{
    /* dbgputs must still work after moving to the private range */
    CHECK(syscall(SYS_dbgputs, (unsigned long)"SYN3: dbgputs alive\n") > 0,
          "dbgputs on private number");
    char kbuf[64];
    CHECK(syscall(SYS_klog, 2UL, (unsigned long)kbuf,
                  (unsigned long)(sizeof kbuf - 1)) >= 0, "klog on private number");

    /* preadv2 / pwritev2 */
    int fd = open("/tmp/syn3file", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "p2: open");
    CHECK(write(fd, "0123456789ABCDEFGHIJ", 20) == 20, "p2: seed");

    char out[32];
    struct iov riov[2] = { { out, 6 }, { out + 6, 4 } };
    long r = syscall(SYS_preadv2, (unsigned long)fd, (unsigned long)riov, 2UL,
                     5L, 0UL);
    CHECK(r == 10, "preadv2 reads 10 bytes at offset 5");
    CHECK(!memcmp(out, "56789ABCD", 9), "preadv2 content");

    /* offset -1 means "at the current position" */
    lseek(fd, 10, SEEK_SET);
    memset(out, 0, sizeof out);
    riov[0].len = 5;
    r = syscall(SYS_preadv2, (unsigned long)fd, (unsigned long)riov, 1UL,
                -1L, 0UL);
    CHECK(r == 5 && !memcmp(out, "ABCDE", 5), "preadv2 offset -1 follows position");

    /* RWF_NOWAIT must be refused, not silently honoured */
    errno = 0;
    CHECK(syscall(SYS_preadv2, (unsigned long)fd, (unsigned long)riov, 1UL, 0L,
                  0x08UL) < 0, "preadv2 refuses RWF_NOWAIT");

    /* pwritev2 with an offset, and RWF_APPEND pinning to EOF */
    struct iov wiov[2] = { { (void*)"xx", 2 }, { (void*)"yy", 2 } };
    CHECK(syscall(SYS_pwritev2, (unsigned long)fd, (unsigned long)wiov, 2UL,
                  20L, 0UL) == 4, "pwritev2 writes 4 at offset 20");
    memset(out, 0, sizeof out);
    pread(fd, out, 4, 20);
    CHECK(!memcmp(out, "xxyy", 4), "pwritev2 content");
    CHECK(syscall(SYS_pwritev2, (unsigned long)fd, (unsigned long)wiov, 1UL,
                  0L, 0x10UL) == 2, "pwritev2 RWF_APPEND");
    memset(out, 0, sizeof out);
    pread(fd, out, 2, 24);
    CHECK(!memcmp(out, "xx", 2), "RWF_APPEND wrote at EOF");
    close(fd);

    /* statx: fields present and the new ones readable */
    struct statx sx;
    memset(&sx, 0, sizeof sx);
    CHECK(syscall(SYS_statx, (long)AT_FDCWD, (unsigned long)"/tmp/syn3file",
                  AT_STATX_SYNC_AS_STAT, 0x0FFFUL,
                  (unsigned long)&sx) == 0, "statx");
    CHECK(sx.mask != 0, "statx mask set");
    printf("  statx size=%llu\n", (unsigned long long)sx.size);
    CHECK(sx.size == 26, "statx size");
    CHECK(sx.blksize != 0, "statx blksize");
    CHECK(sx.nlink >= 1, "statx nlink");
    CHECK(sx.mnt_id != 0, "statx mnt_id populated");
    CHECK(sx.dio_mem_align == 0 && sx.dio_offset_align == 0,
          "statx DIO alignment reported as none");

    /* mount_setattr: refused honestly (no mount table) */
    struct { uint64_t set, clr, prop, userns_fd; } mattr = {0, 0, 0, 0};
    errno = 0;
    r = syscall(SYS_mount_setattr, (long)AT_FDCWD, (unsigned long)"/", 0UL,
                (unsigned long)&mattr, sizeof mattr);
    CHECK(r < 0, "mount_setattr refused (no mount table)");
    CHECK(syscall(SYS_mount_setattr, (long)AT_FDCWD, (unsigned long)"/", 0UL,
                  (unsigned long)&mattr, 4UL) < 0,
          "mount_setattr rejects short struct");

    /* process_madvise: pidfd target, advice validation, byte count */
    pid_t me = getpid();
    int myfd = (int)syscall(SYS_pidfd_open, (unsigned long)me, 0UL);
    CHECK(myfd >= 0, "pmadvise: pidfd_open self");
    struct iov pmi[2] = { { (void*)0x400000UL, 4096 }, { (void*)0x401000UL, 8192 } };
    r = syscall(SYS_process_madvise, (unsigned long)myfd, (unsigned long)pmi,
                2UL, 4UL, 0UL);
    CHECK(r == 12288, "process_madvise returns bytes advised");
    r = syscall(SYS_process_madvise, (unsigned long)myfd, (unsigned long)pmi,
                2UL, 0UL, 0UL);
    CHECK(r == 12288, "process_madvise MADV_NORMAL");
    errno = 0;
    CHECK(syscall(SYS_process_madvise, (unsigned long)myfd, (unsigned long)pmi,
                  2UL, 999UL, 0UL) < 0 && errno == EINVAL,
          "process_madvise rejects unknown advice");
    CHECK(syscall(SYS_process_madvise, (unsigned long)myfd, (unsigned long)pmi,
                  2UL, 4UL, 1UL) < 0, "process_madvise rejects flags");
    CHECK(syscall(SYS_process_madvise, (unsigned long)9999, (unsigned long)pmi,
                  2UL, 4UL, 0UL) < 0, "process_madvise rejects non-pidfd");
    struct iov kernel_range = { (void*)0xFFFF800000000000UL, 4096 };
    CHECK(syscall(SYS_process_madvise, (unsigned long)myfd,
                  (unsigned long)&kernel_range, 1UL, 4UL, 0UL) < 0,
          "process_madvise rejects kernel address");
    close(myfd);

    printf("\n%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
