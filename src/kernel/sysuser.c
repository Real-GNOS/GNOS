/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sysuser.c — credential and permission syscalls. (GPLv2)
 *
 * The user/identity half of the POSIX layer: uid/gid/groups getters and
 * setters, chmod/chown/access/umask, and the owner/permission checks they
 * rely on.  Everything else lives in syscall.c; the dispatch table that
 * reaches both lives in dispatch.c.
 */
#include <stddef.h>
#include <stdint.h>

#include "syscall_impl.h"
#include "syscall.h"
#include "proc.h"
#include "vfs.h"
#include "tmpfs.h"
#include "vmm.h"
#include "panic.h"
#include "kstring.h"
#include "debugcon.h"

/* umask(95): remembered and inherited, but never enforced -- we have no
 * permission checks to enforce it against.  Programs still expect the
 * read-back-and-restore idiom to work. */
static int64_t sys_umask(uint32_t mask)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    uint32_t old = p->umask;
    p->umask     = mask & 0777;
    return (int64_t)old;
}

/* Only the owner and root may change a file's permission bits. */
static int chmod_allowed(const char *abs)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    uint32_t uid, gid, mode;
    int      is_dir;
    int      r = vfs_owner(abs, &uid, &gid, &mode, &is_dir, 1);
    if (r < 0)
        return r;
    (void)gid;
    (void)mode;
    (void)is_dir;

    if (p->euid == 0 || p->euid == uid)
        return 0;
    return -E_PERM;
}

static int64_t sys_chmod(uint64_t upath, uint32_t mode)
{
    char abs[GNUOS_PATH_MAX];
    int  r = path_abs(upath, abs);
    if (r < 0)
        return r;
    r = chmod_allowed(abs);
    if (r < 0)
        return r;
    return vfs_chmod(abs, mode);
}

/*
 * chown(92) / lchown(94) / fchownat(260).  Handing a file to a different user
 * is root's privilege alone; a plain owner may only move a file it already
 * owns into a group it belongs to.  This is the rule that stops a user from
 * dumping a quota-eating file onto somebody else's account.
 */
static int chown_allowed(uint32_t owner, uint32_t uid, uint32_t gid)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    if (p->euid == 0)
        return 0;

    if (uid != (uint32_t)-1 && uid != owner)
        return -E_PERM; /* giving it away: root only */
    if (p->euid != owner)
        return -E_PERM; /* not yours to re-group     */
    if (gid != (uint32_t)-1 && !proc_in_group(p, gid))
        return -E_PERM;
    return 0;
}

static int64_t sys_chown(const char *abs, uint32_t uid, uint32_t gid, int follow)
{
    uint32_t owner, group, mode;
    int      is_dir;
    int      r = vfs_owner(abs, &owner, &group, &mode, &is_dir, follow);
    if (r < 0)
        return r;
    (void)group;
    (void)mode;
    (void)is_dir;

    r = chown_allowed(owner, uid, gid);
    if (r < 0)
        return r;
    return vfs_chown(abs, uid, gid, follow);
}

static int64_t sys_fchown(int fd, uint32_t uid, uint32_t gid)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    (void)p;
    int h = fd_handle(fd);
    if (h < 0)
        return -E_BADF;

    lstat_t st;
    int     r = vfs_fstat(h, &st);
    if (r < 0)
        return r;

    r = chown_allowed(st.st_uid, uid, gid);
    if (r < 0)
        return r;
    return vfs_fchown(h, uid, gid);
}

/*
 * access(21) / faccessat(269).  F_OK (0) asks only whether the name resolves;
 * R_OK/W_OK/X_OK are answered against the *real* uid, not the effective one,
 * which is the whole point of the call: a setuid program uses it to ask "may
 * the user who invoked me touch this?".
 */
static int64_t sys_access(int dfd, uint64_t upath, int at, int amode)
{
    char abs[GNUOS_PATH_MAX];
    int  r = at ? path_at(dfd, upath, abs) : path_abs(upath, abs);
    if (r < 0)
        return r;

    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    uint32_t uid, gid, mode;
    int      is_dir;
    r = vfs_owner(abs, &uid, &gid, &mode, &is_dir, 1);
    if (r < 0)
        return r;

    int want = amode & 7;
    if (!want)
        return 0; /* F_OK: it exists, we are done */

    /* Swap in the real ids for the duration of the test, per POSIX. */
    uint32_t save_euid = p->euid, save_egid = p->egid;
    p->euid = p->uid;
    p->egid = p->gid;
    int ok  = proc_permitted(mode, uid, gid, want, is_dir);
    p->euid = save_euid;
    p->egid = save_egid;

    return ok ? 0 : -E_ACCES;
}

/* ---- credentials -------------------------------------------------------
 *
 * The POSIX id model in full: a real id (who you are), an effective id (what
 * the kernel checks against) and a saved set-user-id (what a privileged
 * program may switch back to after dropping privilege).  A process with
 * euid 0 may set any of them to anything; an unprivileged one may only ever
 * move between the three it already holds, which is precisely the rule that
 * lets `su` drop to a user and never lets a user climb back.
 */
static int64_t sys_getresuid(uint64_t ruid, uint64_t euid, uint64_t suid)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;
    if (ruid && user_ptr_ok(ruid, 4))
        *(uint32_t *)(uintptr_t)ruid = p->uid;
    if (euid && user_ptr_ok(euid, 4))
        *(uint32_t *)(uintptr_t)euid = p->euid;
    if (suid && user_ptr_ok(suid, 4))
        *(uint32_t *)(uintptr_t)suid = p->suid;
    return 0;
}

static int64_t sys_getresgid(uint64_t rgid, uint64_t egid, uint64_t sgid)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;
    if (rgid && user_ptr_ok(rgid, 4))
        *(uint32_t *)(uintptr_t)rgid = p->gid;
    if (egid && user_ptr_ok(egid, 4))
        *(uint32_t *)(uintptr_t)egid = p->egid;
    if (sgid && user_ptr_ok(sgid, 4))
        *(uint32_t *)(uintptr_t)sgid = p->sgid;
    return 0;
}

/* -1 means "leave this one alone", which is how setre*id and setres*id say
 * "change only the other field". */
#define ID_KEEP 0xFFFFFFFFu

/* May an unprivileged process adopt `id` as one of its user ids?  Only if it
 * already holds it as one of the three. */
static int uid_allowed(const proc_t *p, uint32_t id)
{
    return p->euid == 0 || id == p->uid || id == p->euid || id == p->suid;
}

static int gid_allowed(const proc_t *p, uint32_t id)
{
    return p->euid == 0 || id == p->gid || id == p->egid || id == p->sgid;
}

static int64_t sys_setresuid(uint32_t r, uint32_t e, uint32_t s)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;

    if ((r != ID_KEEP && !uid_allowed(p, r)) || (e != ID_KEEP && !uid_allowed(p, e)) ||
        (s != ID_KEEP && !uid_allowed(p, s)))
        return -E_PERM;

    if (r != ID_KEEP)
        p->uid = r;
    if (e != ID_KEEP)
        p->euid = e;
    if (s != ID_KEEP)
        p->suid = s;
    return 0;
}

static int64_t sys_setresgid(uint32_t r, uint32_t e, uint32_t s)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;

    if ((r != ID_KEEP && !gid_allowed(p, r)) || (e != ID_KEEP && !gid_allowed(p, e)) ||
        (s != ID_KEEP && !gid_allowed(p, s)))
        return -E_PERM;

    if (r != ID_KEEP)
        p->gid = r;
    if (e != ID_KEEP)
        p->egid = e;
    if (s != ID_KEEP)
        p->sgid = s;
    return 0;
}

/*
 * setuid(105).  For root this is the irreversible one: all three ids move, so
 * a program that drops to a user cannot climb back.  For anyone else only the
 * effective id moves, which is how a set-user-ID program toggles its
 * privilege on and off.
 */
static int64_t sys_setuid(uint32_t uid)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;

    if (p->euid == 0) {
        p->uid = p->euid = p->suid = uid;
        return 0;
    }
    if (!uid_allowed(p, uid))
        return -E_PERM;
    p->euid = uid;
    return 0;
}

static int64_t sys_setgid(uint32_t gid)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;

    if (p->euid == 0) {
        p->gid = p->egid = p->sgid = gid;
        return 0;
    }
    if (!gid_allowed(p, gid))
        return -E_PERM;
    p->egid = gid;
    return 0;
}

static int64_t sys_setreuid(uint32_t r, uint32_t e)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;

    if ((r != ID_KEEP && !uid_allowed(p, r)) || (e != ID_KEEP && !uid_allowed(p, e)))
        return -E_PERM;

    uint32_t old_uid = p->uid;
    if (r != ID_KEEP)
        p->uid = r;
    if (e != ID_KEEP)
        p->euid = e;
    /* POSIX: if the real uid was changed, or the effective was set to
     * something other than the old real uid, the saved id follows the new
     * effective one. */
    if (r != ID_KEEP || (e != ID_KEEP && e != old_uid))
        p->suid = p->euid;
    return 0;
}

static int64_t sys_setregid(uint32_t r, uint32_t e)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;

    if ((r != ID_KEEP && !gid_allowed(p, r)) || (e != ID_KEEP && !gid_allowed(p, e)))
        return -E_PERM;

    uint32_t old_gid = p->gid;
    if (r != ID_KEEP)
        p->gid = r;
    if (e != ID_KEEP)
        p->egid = e;
    if (r != ID_KEEP || (e != ID_KEEP && e != old_gid))
        p->sgid = p->egid;
    return 0;
}

/* getgroups(115): size 0 is the "how many are there?" probe. */
static int64_t sys_getgroups(int size, uint64_t ulist)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;
    if (size == 0)
        return (int64_t)p->ngroups;
    if (size < 0 || (uint32_t)size < p->ngroups)
        return -E_INVAL;
    if (!user_ptr_ok(ulist, p->ngroups * 4))
        return -E_FAULT;

    uint32_t *out = (uint32_t *)(uintptr_t)ulist;
    for (uint32_t i = 0; i < p->ngroups; i++)
        out[i] = p->groups[i];
    return (int64_t)p->ngroups;
}

static int64_t sys_setgroups(int size, uint64_t ulist)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_SRCH;
    if (p->euid != 0)
        return -E_PERM;
    if (size < 0 || size > PROC_NGROUPS)
        return -E_INVAL;
    if (size && !buf_ok(ulist, (uint64_t)size * 4, 0))
        return -E_FAULT;

    const uint32_t *in = (const uint32_t *)(uintptr_t)ulist;
    for (int i = 0; i < size; i++)
        p->groups[i] = in[i];
    p->ngroups = (uint32_t)size;
    return 0;
}

/* ---- dispatch entry points --------------------------------------------
 * Generated from the old switch by tools/case2handler.c.  One uniform
 * entry point per syscall number; see syscall_list.h for the table. */

/* Generated by tools/case2handler.c -- append to sysuser.c. */

int64_t __x64_sys_umask(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_umask((uint32_t)a1);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_chmod(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_chmod(a1, (uint32_t)a2);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_access(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_access(AT_FDCWD, a1, 0, (int)a2);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_faccessat(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_access((int)a1, a2, 1, (int)a3);
            break;
    } while (0);
    return ret;
}

/* faccessat2(439): musl's faccessat() on a kernel that advertises it. */
int64_t __x64_sys_faccessat2(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_access((int)a1, a2, 1, (int)a3);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_chown(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
        {
            char abs[GNUOS_PATH_MAX];
            ret = path_abs(a1, abs);
            if (ret < 0)
                break;
            ret = sys_chown(abs, (uint32_t)a2, (uint32_t)a3, nr == SYS_chown);
            break;
        }
    } while (0);
    return ret;
}

int64_t __x64_sys_fchown(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_fchown((int)a1, (uint32_t)a2, (uint32_t)a3);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_fchmod(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
        {
            /* fchmod(fd, mode): wlroots' shm files go through this with mode 0
             * to lock a shared-memory file down before use. */
            int         h  = fd_handle((int)a1);
            vfs_node_t *tn = (h >= 0) ? vfs_file_node(h) : NULL;
            const char *fp = (h >= 0) ? vfs_file_path(h) : NULL;
            if (!fp) {
                ret = -E_BADF;
                break;
            }
            /* tmpfs files are world-writable scratch (/tmp, /run, /dev/shm):
             * the mode is meaningless, and resolving by path fails for an
             * object that was unlinked while still open.  Report success. */
            if (tmpfs_is_file_node(tn)) {
                ret = 0;
                break;
            }
            ret = chmod_allowed(fp);
            if (ret < 0)
                break;
            ret = vfs_chmod(fp, (uint32_t)a2);
            break;
        }
    } while (0);
    return ret;
}

int64_t __x64_sys_fchmodat(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
        {
            char abs[GNUOS_PATH_MAX];
            ret = path_at((int)a1, a2, abs);
            if (ret < 0)
                break;
            /* AT_SYMLINK_NOFOLLOW (0x100) would mean chmod the link itself, which
             * we cannot do; we always operate on the resolved target. */
            ret = chmod_allowed(abs);
            if (ret < 0)
                break;
            ret = vfs_chmod(abs, (uint32_t)a3);
            break;
        }
    } while (0);
    return ret;
}

int64_t __x64_sys_fchownat(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
        {
            char abs[GNUOS_PATH_MAX];
            ret = path_at((int)a1, a2, abs);
            if (ret < 0)
                break;
            /* AT_SYMLINK_NOFOLLOW (0x100) picks the link itself. */
            ret = sys_chown(abs, (uint32_t)a3, (uint32_t)r->r10, !(r->r8 & 0x100));
            break;
        }
    } while (0);
    return ret;
}

/* ---- credentials --------------------------------------------------- */
int64_t __x64_sys_getuid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = proc_current() ? (int64_t)proc_current()->uid : 0;
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_geteuid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = proc_current() ? (int64_t)proc_current()->euid : 0;
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_getgid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = proc_current() ? (int64_t)proc_current()->gid : 0;
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_getegid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = proc_current() ? (int64_t)proc_current()->egid : 0;
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_setuid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_setuid((uint32_t)a1);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_setgid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_setgid((uint32_t)a1);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_setreuid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_setreuid((uint32_t)a1, (uint32_t)a2);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_setregid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_setregid((uint32_t)a1, (uint32_t)a2);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_setresuid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_setresuid((uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_setresgid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_setresgid((uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_getgroups(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_getgroups((int)a1, a2);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_setgroups(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_setgroups((int)a1, a2);
            break;
    } while (0);
    return ret;
}

/* Each argument may be NULL, which we must not write through. */
int64_t __x64_sys_getresuid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_getresuid(a1, a2, a3);
            break;
    } while (0);
    return ret;
}

int64_t __x64_sys_getresgid(regs_t *r)
{
    uint64_t nr  __attribute__((unused)) = r->rax;
    uint64_t a1  __attribute__((unused)) = r->rdi;
    uint64_t a2  __attribute__((unused)) = r->rsi;
    uint64_t a3  __attribute__((unused)) = r->rdx;
    proc_t  *p   __attribute__((unused)) = proc_current();
    int64_t  ret = 0;
    do {
            ret = sys_getresgid(a1, a2, a3);
            break;
    } while (0);
    return ret;
}
