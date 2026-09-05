/*
 * sysvipc.h — System V IPC (message queues, semaphores, shared memory).
 * (GPLv2)
 *
 * The kernel objects mirror Linux's semantics closely enough for musl (and
 * programs built against it) to work unchanged:
 *
 *   - lookup by key with IPC_CREAT/IPC_EXCL/IPC_PRIVATE and the classic
 *     permission check against the creator/owner credentials;
 *   - message queues: msgsnd blocks (or EAGAINs with IPC_NOWAIT) while the
 *     queue is full, msgrcv blocks (or ENOMSG) until a message of the
 *     requested type arrives, MSG_NOERROR truncates, and IPC_RMID wakes
 *     every sleeper with EIDRM;
 *   - semaphores: atomic semop over the whole sops array, per-semaphore
 *     blocking with semncnt/semzcnt bookkeeping, SEM_UNDO adjustments that
 *     are rolled back when the process exits, and the full semctl command
 *     set (GETPID/GETVAL/GETALL/GETNCNT/GETZCNT/SETVAL/SETALL);
 *   - shared memory: a segment is a private set of frames mapped into each
 *     attaching process at shmat() time; shmctl(IPC_RMID) marks it for
 *     destruction and the frames are freed when the last attachment goes
 *     away (shmdt or process exit).  Attached pages carry an x86 PTE
 *     "available" bit so address-space teardown and munmap clear them
 *     without freeing the frames the segment still owns.
 *
 * All object tables are fixed static pools in the style of the proc table;
 * identifiers are monotonic (slots are not reused) so a stale id can never
 * name a live object of another kind.  The module needs no locks of its
 * own: user syscalls are serialised by the big kernel lock, and sleeping
 * (sched_block) hands the lock to whoever wakes the sleeper, so every
 * check-then-act below is atomic against other processes.
 *
 * The structs below are byte-for-byte copies of musl's x86-64 ABI layouts
 * (arch/generic/bits/ipc.h + msg.h, arch/x86_64/bits/sem.h, shm.h); the
 * IPC_STAT/IPC_SET commands memcpy whole structs across the user boundary,
 * so a size mismatch would corrupt the caller's memory.
 */
#ifndef GNUCOS_SYSVIPC_H
#define GNUCOS_SYSVIPC_H

#include <stddef.h>
#include <stdint.h>

#include "proc.h"

/* ---- ABI constants (Linux numbers, matching the musl headers) ----------- */
/* IPC_PRIVATE: key 0 never looks up an existing object; every get creates. */
#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000

#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3

/* semctl commands beyond IPC_* */
#define SEM_UNDO    0x1000
#define GETPID      11
#define GETVAL      12
#define GETALL      13
#define GETNCNT     14
#define GETZCNT     15
#define SETVAL      16
#define SETALL      17

/* msgrcv/msgsnd flags beyond IPC_* */
#define MSG_NOERROR 010000
#define MSG_EXCEPT  020000

/* shmat flags */
#define SHM_RDONLY  010000
#define SHM_RND     020000

/* ---- ABI structs (kernel copies of the musl layouts) -------------------- */
typedef struct {
    int32_t  key;
    int32_t  uid, gid, cuid, cgid;
    uint32_t mode;
    int32_t  seq;
    int64_t  pad1, pad2;
} kipc_perm_t;

typedef struct {
    kipc_perm_t perm;
    int64_t     stime, rtime, ctime;
    uint64_t    cbytes, qnum, qbytes;
    int32_t     lspid, lrpid;
    int64_t     unused[2];
} kmsqid_ds_t;

typedef struct {
    kipc_perm_t perm;
    int64_t     otime;
    int64_t     unused1;
    int64_t     ctime;
    int64_t     unused2;
    uint16_t    nsems;
    char        nsems_pad[6];
    int64_t     unused3, unused4;
} ksemid_ds_t;

typedef struct {
    kipc_perm_t perm;
    uint64_t    segsz;
    int64_t     atime, dtime, ctime;
    int32_t     cpid, lpid;
    uint64_t    nattch;
    uint64_t    pad1, pad2;
} kshmid_ds_t;

_Static_assert(sizeof(kipc_perm_t) == 48, "ipc_perm ABI size");
_Static_assert(sizeof(kmsqid_ds_t) == 120, "msqid_ds ABI size");
_Static_assert(sizeof(ksemid_ds_t) == 104, "semid_ds ABI size");
_Static_assert(sizeof(kshmid_ds_t) == 112, "shmid_ds ABI size");

/* ---- syscall surface ---------------------------------------------------- */
/* Message queues */
long sysv_msgget(int32_t key, int flags);
long sysv_msgsnd(int msqid, uint64_t umsgp, size_t msgsz, int flags);
long sysv_msgrcv(int msqid, uint64_t umsgp, size_t msgsz, long msgtyp,
                 int flags);
long sysv_msgctl(int msqid, int cmd, uint64_t ubuf);

/* Semaphores */
long sysv_semget(int32_t key, int nsems, int flags);
long sysv_semop(int semid, uint64_t usops, size_t nsops);
long sysv_semtimedop(int semid, uint64_t usops, size_t nsops,
                     uint64_t utimeout);
long sysv_semctl(int semid, int semnum, int cmd, uint64_t uarg);

/* Shared memory */
long sysv_shmget(int32_t key, size_t size, int flags);
long sysv_shmat(int shmid, uint64_t uaddr, int flags);
long sysv_shmdt(uint64_t uaddr);
long sysv_shmctl(int shmid, int cmd, uint64_t ubuf);

/* Called from proc_teardown: roll back SEM_UNDO adjustments and detach
 * every shared-memory segment the exiting process still has attached. */
void sysv_exit(proc_t *p);

/* Called from execve before the old address space is dropped: detach every
 * shared-memory segment (the new image maps nothing of the old space) but
 * keep SEM_UNDO adjustments, which Linux carries across exec. */
void sysv_exec_reset(proc_t *p);

#endif
