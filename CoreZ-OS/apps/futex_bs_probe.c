#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_MATCH_ANY 0xffffffffu
#define SYS_FUTEX 202

static int ops_c, ops_p, bs_c, bs_p, pt_c, pt_p;

#define CK(gc, gp, c)                                                          \
    do {                                                                       \
        gc++;                                                                  \
        if (c)                                                                 \
            gp++;                                                              \
        else                                                                   \
            printf("  futex_bs fail@%d\n", __LINE__);                          \
    } while (0)

static long futex6(volatile int *addr, int op, int val, void *ts, unsigned bits) {
    return syscall(SYS_FUTEX, (unsigned long)addr,
                   (unsigned long)(op | FUTEX_PRIVATE), (unsigned long)val,
                   (unsigned long)ts, 0UL, (unsigned long)bits);
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static long long now_ms(int clockid) {
    struct timespec ts;
    if (clock_gettime(clockid, &ts) != 0)
        return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_abs(struct timespec *ts, long long when_ms) {
    ts->tv_sec = (time_t)(when_ms / 1000);
    ts->tv_nsec = (long)((when_ms % 1000) * 1000000);
}

static volatile int shared_word;
static volatile int woke_flag;
static volatile long woke_ret;
static unsigned waiter_bits;

static void *bitset_waiter(void *arg) {
    (void)arg;
    woke_ret = futex6(&shared_word, FUTEX_WAIT_BITSET, 0, 0, waiter_bits);
    woke_flag = 1;
    return 0;
}

static void *timed_waiter(void *arg) {
    (void)arg;
    struct timespec ts;
    set_abs(&ts, now_ms(CLOCK_MONOTONIC) + 4000);
    woke_ret = futex6(&shared_word, FUTEX_WAIT_BITSET, 0, &ts, FUTEX_MATCH_ANY);
    woke_flag = 1;
    return 0;
}

static void *trivial_thread(void *arg) {
    (void)arg;
    write(1, "fbs t0: child ran\n", 18);
    return (void *)7;
}

static void test_trivial_pthread(void) {
    pthread_t th;
    void *rv = 0;
    printf("fbs t0: create trivial thread\n");
    int rc = pthread_create(&th, 0, trivial_thread, 0);
    CK(pt_c, pt_p, rc == 0);
    if (rc != 0)
        return;
    sleep_ms(120);
    CK(pt_c, pt_p, pthread_join(th, &rv) == 0);
    CK(pt_c, pt_p, (long)rv == 7);
    printf("fbs t0: joined rv=%d\n", (int)(long)rv);
}

static void test_ops(void) {
    volatile int fut = 7;
    struct timespec ts;
    long r;
    long long t0, t1;

    printf("fbs t1: eagain\n");
    r = futex6(&fut, FUTEX_WAIT, 8, 0, 0);
    CK(ops_c, ops_p, r == -1 && errno == EAGAIN);

    ts.tv_sec = 0;
    ts.tv_nsec = 60000000L;
    printf("fbs t2: rel timeout 60ms\n");
    t0 = now_ms(CLOCK_MONOTONIC);
    r = futex6(&fut, FUTEX_WAIT, 7, &ts, 0);
    t1 = now_ms(CLOCK_MONOTONIC);
    printf("fbs t2: r=%ld errno=%d dt=%lld\n", r, errno, t1 - t0);
    CK(ops_c, ops_p, r == -1 && errno == ETIMEDOUT);
    CK(ops_c, ops_p, t1 - t0 >= 40 && t1 - t0 < 800);

    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    printf("fbs t3: zero timeout\n");
    r = futex6(&fut, FUTEX_WAIT, 7, &ts, 0);
    printf("fbs t3: r=%ld errno=%d\n", r, errno);
    CK(ops_c, ops_p, r == -1 && errno == ETIMEDOUT);

    set_abs(&ts, now_ms(CLOCK_MONOTONIC) + 60);
    printf("fbs t4: abs monotonic\n");
    t0 = now_ms(CLOCK_MONOTONIC);
    r = futex6(&fut, FUTEX_WAIT_BITSET, 7, &ts, FUTEX_MATCH_ANY);
    t1 = now_ms(CLOCK_MONOTONIC);
    printf("fbs t4: r=%ld errno=%d dt=%lld\n", r, errno, t1 - t0);
    CK(ops_c, ops_p, r == -1 && errno == ETIMEDOUT);
    CK(ops_c, ops_p, t1 - t0 >= 40 && t1 - t0 < 800);

    set_abs(&ts, now_ms(CLOCK_REALTIME) + 600);
    printf("fbs t5: abs realtime\n");
    r = futex6(&fut, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 7, &ts,
               FUTEX_MATCH_ANY);
    printf("fbs t5: r=%ld errno=%d\n", r, errno);
    CK(ops_c, ops_p, r == -1 && errno == ETIMEDOUT);

    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    printf("fbs t6: zero bitset\n");
    r = futex6(&fut, FUTEX_WAIT_BITSET, 7, &ts, 0);
    printf("fbs t6: r=%ld errno=%d\n", r, errno);
    CK(ops_c, ops_p, r == -1 && errno == EINVAL);

    printf("fbs t7: unknown op\n");
    r = futex6(&fut, 5, 0, 0, 0);
    printf("fbs t7: r=%ld errno=%d\n", r, errno);
    CK(ops_c, ops_p, r == -1 && errno == ENOSYS);

    printf("fbs t8: wake none\n");
    r = futex6(&fut, FUTEX_WAKE, 1, 0, 0);
    printf("fbs t8: r=%ld\n", r);
    CK(ops_c, ops_p, r == 0);

    printf("fbs t9: null addr\n");
    r = futex6(0, FUTEX_WAIT, 0, 0, 0);
    printf("fbs t9: r=%ld errno=%d\n", r, errno);
    CK(ops_c, ops_p, r != 0);
}

static void test_bitset(void) {
    pthread_t th;
    long r;

    shared_word = 0;
    woke_flag = 0;
    woke_ret = -999;
    waiter_bits = 1;
    printf("fbs b1: create waiter bits=1\n");
    if (pthread_create(&th, 0, bitset_waiter, 0) != 0) {
        printf("  futex_bs: pthread_create failed\n");
        return;
    }
    sleep_ms(400);
    printf("fbs b1: wake bits=2 (expect 0)\n");
    r = futex6(&shared_word, FUTEX_WAKE_BITSET, 1, 0, 2);
    printf("fbs b1: r=%ld\n", r);
    CK(bs_c, bs_p, r == 0);
    sleep_ms(200);
    CK(bs_c, bs_p, woke_flag == 0);
    printf("fbs b1: wake bits=1 (expect 1)\n");
    r = futex6(&shared_word, FUTEX_WAKE_BITSET, 1, 0, 1);
    printf("fbs b1: r=%ld\n", r);
    CK(bs_c, bs_p, r == 1);
    sleep_ms(300);
    CK(bs_c, bs_p, woke_flag == 1 && woke_ret == 0);
    pthread_join(th, 0);

    shared_word = 0;
    woke_flag = 0;
    woke_ret = -999;
    waiter_bits = 4;
    printf("fbs b2: create waiter bits=4\n");
    if (pthread_create(&th, 0, bitset_waiter, 0) != 0)
        return;
    sleep_ms(400);
    printf("fbs b2: plain wake\n");
    r = futex6(&shared_word, FUTEX_WAKE, 1, 0, 0);
    printf("fbs b2: r=%ld\n", r);
    CK(bs_c, bs_p, r == 1);
    sleep_ms(300);
    CK(bs_c, bs_p, woke_flag == 1 && woke_ret == 0);
    pthread_join(th, 0);
}

static void test_pthread_timed(void) {
    pthread_t th;
    long r;
    long long t0, t1;

    shared_word = 0;
    woke_flag = 0;
    woke_ret = -999;
    waiter_bits = FUTEX_MATCH_ANY;
    printf("fbs p1: create timed waiter\n");
    if (pthread_create(&th, 0, timed_waiter, 0) != 0)
        return;
    t0 = now_ms(CLOCK_MONOTONIC);
    sleep_ms(300);
    printf("fbs p1: wake before deadline\n");
    r = futex6(&shared_word, FUTEX_WAKE_BITSET, 1, 0, FUTEX_MATCH_ANY);
    t1 = now_ms(CLOCK_MONOTONIC);
    printf("fbs p1: r=%ld dt=%lld\n", r, t1 - t0);
    CK(pt_c, pt_p, r == 1);
    pthread_join(th, 0);
    CK(pt_c, pt_p, woke_flag == 1 && woke_ret == 0);
    CK(pt_c, pt_p, t1 - t0 < 2000);

    shared_word = 0;
    woke_flag = 0;
    woke_ret = -999;
    printf("fbs p2: create waiter, let it time out\n");
    if (pthread_create(&th, 0, timed_waiter, 0) != 0)
        return;
    sleep_ms(1200);
    printf("fbs p2: woke=%d ret=%ld\n", woke_flag, woke_ret);
    CK(pt_c, pt_p, woke_flag == 0);
    sleep_ms(4000);
    printf("fbs p2: after deadline woke=%d ret=%ld\n", woke_flag, woke_ret);
    CK(pt_c, pt_p, woke_flag == 1 && woke_ret == -1);
    pthread_join(th, 0);
}

int main(void) {
    printf("futex_bs_probe: start\n");
    test_trivial_pthread();
    printf("fbs t0 done\n");
    test_ops();
    printf("fbs ops done %d/%d\n", ops_p, ops_c);
    test_bitset();
    printf("fbs bitset done %d/%d\n", bs_p, bs_c);
    test_pthread_timed();
    printf("fbs pthread done %d/%d\n", pt_p, pt_c);
    printf("futex_bs: ops %d/%d bitset %d/%d pthread %d/%d\n", ops_p, ops_c,
           bs_p, bs_c, pt_p, pt_c);
    int all = (ops_p == ops_c) && (bs_p == bs_c) && (pt_p == pt_c);
    printf("futex_bs_probe: %s %d/%d\n", all ? "PASS" : "FAIL",
           ops_p + bs_p + pt_p, ops_c + bs_c + pt_c);
    return all ? 0 : 1;
}
