/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/wait.h>
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
#define SYS_quotactl_fd   443
#define SYS_memfd_secret  447
#define SYS_futex_waitv   449
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule       445
#define SYS_landlock_restrict_self  446
#define SYS_process_mrelease        448
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_PRIVATE_FLAG 0x80
#define QCMD(cmd, type)   (((cmd) << 8) | ((type) & 0xff))
#define Q_GETQUOTA        0x800007
#define Q_SYNC            0x800001

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
static uint32_t *g_waitv_target;

static void *waitv_bumper(void *unused)
{
    (void)unused;
    usleep(150000);
    *g_waitv_target = 100;
    syscall(SYS_futex, (unsigned long)g_waitv_target,
            (unsigned long)FUTEX_WAKE, 1UL, 0UL, 0UL, 0UL);
    return 0;
}
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

    /* quotactl_fd: descriptor, subcommand and quota type are validated;
     * the operation itself is refused (no quota accounting in GNOS) */
    int qfd = open("/tmp/syn3file", O_RDONLY);
    CHECK(qfd >= 0, "quotactl: open");
    errno = 0;
    r = syscall(SYS_quotactl_fd, (unsigned long)qfd,
                (unsigned long)QCMD(Q_GETQUOTA, 0), 0UL, 0UL);
    printf("  quotactl_fd r=%ld errno=%d (EOPNOTSUPP=%d ENOTSUP=%d)\n", r, errno,
           EOPNOTSUPP, ENOTSUP);
    CHECK(r < 0 && (errno == EOPNOTSUPP || errno == ENOTSUP),
          "quotactl_fd Q_GETQUOTA -> not supported");
    CHECK(syscall(SYS_quotactl_fd, (unsigned long)qfd,
                  (unsigned long)QCMD(Q_SYNC, 1), 0UL, 0UL) < 0,
          "quotactl_fd Q_SYNC -> refused");
    errno = 0;
    CHECK(syscall(SYS_quotactl_fd, (unsigned long)qfd, 0x1234UL, 0UL, 0UL) < 0 &&
          errno == EINVAL, "quotactl_fd rejects unknown subcommand");
    errno = 0;
    CHECK(syscall(SYS_quotactl_fd, (unsigned long)qfd,
                  (unsigned long)QCMD(Q_GETQUOTA, 7), 0UL, 0UL) < 0 &&
          errno == EINVAL, "quotactl_fd rejects unknown quota type");
    CHECK(syscall(SYS_quotactl_fd, (unsigned long)9999,
                  (unsigned long)QCMD(Q_GETQUOTA, 0), 0UL, 0UL) < 0,
          "quotactl_fd rejects bad fd");
    close(qfd);

    /* memfd_secret: refused (no way to hide pages from the direct map),
     * and a non-zero flags word is a misuse */
    errno = 0;
    r = syscall(SYS_memfd_secret, 0UL);
    printf("  memfd_secret r=%ld errno=%d (ENOSYS=%d)\n", r, errno, ENOSYS);
    CHECK(r < 0 && errno == ENOSYS, "memfd_secret -> ENOSYS");
    errno = 0;
    CHECK(syscall(SYS_memfd_secret, 0x1UL) < 0 && errno == EINVAL,
          "memfd_secret rejects flags");

    /* futex_waitv: waits on several words, wakes on the one that moves */
    struct futex_waitv { uint64_t val, uaddr; uint32_t flags, reserved; };
    /* the words must live in memory a forked child can also change */
    uint32_t *words = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(words != MAP_FAILED, "futex_waitv: shared mapping");
    if (words == MAP_FAILED) { printf("\n%d tests, %d failures\n", tests, fails); return 1; }
    uint32_t *fw0p = &words[0], *fw1p = &words[1];
    *fw0p = 7; *fw1p = 9;
    struct futex_waitv fv[2];
    memset(&fv, 0, sizeof fv);
    fv[0].val = 7; fv[0].uaddr = (uint64_t)(uintptr_t)fw0p; fv[0].flags = 2;
    fv[1].val = 9; fv[1].uaddr = (uint64_t)(uintptr_t)fw1p; fv[1].flags = 2;

    /* already changed -> returns that index without sleeping */
    *fw1p = 42;
    r = syscall(SYS_futex_waitv, (unsigned long)fv, 2UL, 0UL, 0UL, 1UL);
    CHECK(r == 1, "futex_waitv returns the already-changed index");
    *fw1p = 9;

    /* nothing changed, 200 ms timeout -> ETIMEDOUT */
    errno = 0;
    struct { int64_t sec, nsec; } fts = { 0, 200000000LL };
    r = syscall(SYS_futex_waitv, (unsigned long)fv, 2UL, 0UL,
                (unsigned long)&fts, 1UL);
    CHECK(r < 0 && errno == ETIMEDOUT, "futex_waitv times out");

    /* a thread in the same address space bumps fw0: the wait must come
     * back with index 0 (cross-process wakes need a shared-mapped word,
     * which anonymous MAP_SHARED does not give us here) */
    g_waitv_target = fw0p;
    {
        pthread_t th;
        pthread_create(&th, 0, waitv_bumper, 0);
        r = syscall(SYS_futex_waitv, (unsigned long)fv, 2UL, 0UL, 0UL, 1UL);
        CHECK(r == 0, "futex_waitv woke on the word that changed");
        pthread_join(th, 0);
    }

    /* validation */
    CHECK(syscall(SYS_futex_waitv, (unsigned long)fv, 0UL, 0UL, 0UL, 1UL) < 0,
          "futex_waitv rejects nr=0");
    CHECK(syscall(SYS_futex_waitv, (unsigned long)fv, 2UL, 1UL, 0UL, 1UL) < 0,
          "futex_waitv rejects flags");

    /* anonymous MAP_SHARED must really be shared across a fork */
    {
        uint32_t *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(sh != MAP_FAILED, "shared anon: mmap");
        sh[0] = 0xA5A5;
        pid_t c2 = fork();
        if (c2 == 0) {
            /* the child must see the parent's word through the same frame */
            _exit(sh[0] == 0xA5A5 ? 3 : 4);
        }
        int st2 = 0;
        waitpid(c2, &st2, 0);
        CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 3,
              "shared anon: child sees the parent's write");
        munmap(sh, 4096);
    }

    /* a futex in shared anon memory must be woken across processes */
    {
        uint32_t *fw = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(fw != MAP_FAILED, "shared futex: mmap");
        *fw = 5;
        struct futex_waitv wv1;
        memset(&wv1, 0, sizeof wv1);
        wv1.val = 5; wv1.uaddr = (uint64_t)(uintptr_t)fw; wv1.flags = 2;
        pid_t c3 = fork();
        if (c3 == 0) {
            usleep(150000);
            *fw = 6;
            syscall(SYS_futex, (unsigned long)fw, (unsigned long)FUTEX_WAKE,
                    1UL, 0UL, 0UL, 0UL);
            _exit(0);
        }
        r = syscall(SYS_futex_waitv, (unsigned long)&wv1, 1UL, 0UL, 0UL, 1UL);
        CHECK(r == 0, "shared futex: cross-process wake");
        int st3 = 0; waitpid(c3, &st3, 0);
        munmap(fw, 4096);
    }

    /* landlock + process_mrelease: refused (no LSM, no OOM reaper), but the
     * argument contract is real */
    struct { uint64_t handled; } lla;
    memset(&lla, 0, sizeof lla);
    errno = 0;
    CHECK(syscall(SYS_landlock_create_ruleset, (unsigned long)&lla,
                  sizeof lla, 0UL) < 0 && errno == ENOSYS,
          "landlock_create_ruleset -> ENOSYS");
    errno = 0;
    CHECK(syscall(SYS_landlock_create_ruleset, (unsigned long)&lla, 0UL, 0UL) < 0 &&
          errno == EINVAL, "landlock_create_ruleset rejects size 0");
    errno = 0;
    CHECK(syscall(SYS_landlock_add_rule, (unsigned long)9999, 1UL, 0UL, 0UL) < 0 &&
          errno == EBADF, "landlock_add_rule rejects bad fd");
    errno = 0;
    CHECK(syscall(SYS_landlock_add_rule, (unsigned long)9999, 99UL, 0UL, 0UL) < 0 &&
          errno == EINVAL, "landlock_add_rule rejects unknown rule type");
    errno = 0;
    CHECK(syscall(SYS_landlock_restrict_self, (unsigned long)9999, 1UL) < 0 &&
          errno == EINVAL, "landlock_restrict_self rejects flags");
    errno = 0;
    CHECK(syscall(SYS_process_mrelease, (unsigned long)9999, 0UL) < 0 &&
          errno == EBADF, "process_mrelease rejects non-pidfd");
    {
        int selffd = (int)syscall(SYS_pidfd_open, (unsigned long)getpid(), 0UL);
        CHECK(selffd >= 0, "process_mrelease: pidfd_open self");
        errno = 0;
        CHECK(syscall(SYS_process_mrelease, (unsigned long)selffd, 0UL) < 0 &&
              errno == ENOSYS, "process_mrelease -> ENOSYS (no reaper)");
        close(selffd);
    }

    printf("\n%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
