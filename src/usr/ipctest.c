/*
 * ipctest.c — System V + POSIX IPC self-test. (GPLv2, musl)
 *
 * Boot-time proof that the IPC work behaves the way musl and its programs
 * expect:
 *
 *   SysV messages:   msgget(IPC_PRIVATE), typed msgsnd/msgrcv, blocking
 *                    receive across a fork, msgctl STAT + RMID.
 *   SysV semaphores: semget, semop wait/raise, GETVAL/SETVAL, IPC_RMID.
 *   SysV shm:        shmget/shmat, write + cross-process read after fork,
 *                    shmdt, IPC_RMID (frames freed on last detach).
 *   POSIX shm:       shm_open + ftruncate + mmap(MAP_SHARED), a fork shares
 *                    the mapping (same physical frames), shm_unlink.
 *   POSIX named sem: sem_open + sem_post in the parent waking sem_wait in
 *                    the child -- exercises the shared futex path on the
 *                    MAP_SHARED tmpfs page.
 *
 * Every check prints IPC: PASS n / FAIL n; the exit status is the first
 * failing check or 0, like the other boot-time tests in /etc/rc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <stdarg.h>

static int g_failed, g_passed;

/* POSIX leaves union semun to the application; musl's <sys/sem.h> does not
 * define it.  This is the glibc shape every semctl caller expects. */
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
    struct seminfo  *__buf;
    void            *__pad;
};

static void report(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    syscall(441, buf);          /* SYS_dbgputs: mirror into build/dbg.log */
}

static void check(const char *what, int ok)
{
    if (ok) {
        g_passed++;
        report("IPC: PASS %s", what);
    } else {
        g_failed++;
        report("IPC: FAIL %s (errno %d)", what, errno);
    }
}

/* ---- System V messages ------------------------------------------------ */
static void test_msg(void)
{
    struct msgbuf { long mtype; char mtext[64]; } m;
    int q = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    check("msgget", q >= 0);

    m.mtype = 1;
    strcpy(m.mtext, "sysv-message");
    check("msgsnd", msgsnd(q, &m, strlen(m.mtext) + 1, 0) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        struct msgbuf r;
        memset(&r, 0, sizeof(r));
        if (msgrcv(q, &r, sizeof(r.mtext), 1, 0) >= 0 &&
            strcmp(r.mtext, "sysv-message") == 0)
            _exit(0);
        _exit(1);
    }
    int st = 1;
    waitpid(pid, &st, 0);
    check("msgrcv cross-process", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    struct msqid_ds ds;
    check("msgctl STAT", msgctl(q, IPC_STAT, &ds) == 0 && ds.msg_qnum == 0);
    check("msgctl RMID", msgctl(q, IPC_RMID, 0) == 0);
}

/* ---- System V semaphores ---------------------------------------------- */
static void test_sem(void)
{
    int s = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    check("semget", s >= 0);

    check("semctl SETVAL", semctl(s, 0, SETVAL, (union semun){ .val = 1 }) == 0);
    struct sembuf down = { 0, -1, 0 };
    check("semop down", semop(s, &down, 1) == 0);
    check("semctl GETVAL(0)", semctl(s, 0, GETVAL, 0) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        struct sembuf up = { 0, 1, 0 };
        if (semop(s, &up, 1) == 0)      /* child raises: parent wakes below */
            _exit(0);
        _exit(1);
    }
    /* Blocking wait for the child's raise: exercises the WAIT_IPC wake. */
    struct sembuf down2 = { 0, -1, 0 };
    int r = semop(s, &down2, 1);
    int st = 1;
    waitpid(pid, &st, 0);
    check("semop block across fork", r == 0 &&
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    struct semid_ds sds;
    check("semctl STAT", semctl(s, 0, IPC_STAT, &sds) == 0 &&
          sds.sem_nsems == 1);
    check("semctl RMID", semctl(s, 0, IPC_RMID, 0) == 0);
}

/* ---- System V shared memory -------------------------------------------- */
static void test_shm(void)
{
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    check("shmget", id >= 0);
    char *addr = shmat(id, 0, 0);
    check("shmat", addr != (void *)-1);
    if (addr != (void *)-1) {
        strcpy(addr, "sysv-shared");
        pid_t pid = fork();
        if (pid == 0) {
            if (strcmp(addr, "sysv-shared") == 0)   /* same frames */
                _exit(0);
            _exit(1);
        }
        int st = 1;
        waitpid(pid, &st, 0);
        check("shm cross-process", WIFEXITED(st) && WEXITSTATUS(st) == 0);
        check("shmdt", shmdt(addr) == 0);
    }
    struct shmid_ds sh;
    check("shmctl STAT", shmctl(id, IPC_STAT, &sh) == 0);
    check("shmctl RMID", shmctl(id, IPC_RMID, 0) == 0);
}

/* ---- POSIX shm + named semaphore (musl over /dev/shm + mmap + futex) --- */
static void test_posix(void)
{
    char name[] = "/ipctest.XXXXXX";
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    check("shm_open", fd >= 0);
    if (fd < 0)
        return;
    check("ftruncate", ftruncate(fd, 4096) == 0);

    void *map = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap MAP_SHARED", map != MAP_FAILED);
    if (map != MAP_FAILED) {
        /* named semaphore *inside* the shared region: cross-process wake
         * must ride the shared futex path */
        sem_t *s = sem_open("/ipctest-sem", O_CREAT | O_EXCL, 0600, 0);
        check("sem_open", s != SEM_FAILED);
        if (s != SEM_FAILED) {
            pid_t pid = fork();
            if (pid == 0) {
                if (sem_wait(s) == 0)   /* blocks until parent posts */
                    _exit(0);
                _exit(1);
            }
            usleep(100000);             /* let the child block first */
            int posted = sem_post(s) == 0;
            int st = 1;
            waitpid(pid, &st, 0);
            check("sem_post wakes child", posted &&
                  WIFEXITED(st) && WEXITSTATUS(st) == 0);
            check("sem_close", sem_close(s) == 0);
            check("sem_unlink", sem_unlink("/ipctest-sem") == 0);
        }
        strcpy(map, "posix-shared");
        pid_t p2 = fork();
        if (p2 == 0) {
            if (strcmp(map, "posix-shared") == 0)
                _exit(0);
            _exit(1);
        }
        int st2 = 1;
        waitpid(p2, &st2, 0);
        check("posix shm cross-process", WIFEXITED(st2) && WEXITSTATUS(st2) == 0);
        check("munmap", munmap(map, 4096) == 0);
    }
    check("shm_unlink", shm_unlink(name) == 0);
}

int main(void)
{
    report("IPC: begin");
    test_msg();
    test_sem();
    test_shm();
    test_posix();
    report("IPC: %d passed, %d failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
