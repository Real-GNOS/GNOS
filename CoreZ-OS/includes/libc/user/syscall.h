#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>
#include "kernel/syscall_nr.h"
#include "kernel/nt_ping_reply.h"

struct FS_STAT;
struct FS_DIR;
struct FS_DIRENT;

struct SYS_TIMESPEC {
    int32_t tv_sec;
    int32_t tv_nsec;
};
struct SYS_TIMEVAL {
    int32_t tv_sec;
    int32_t tv_usec;
};
struct LINUX_DIRENT {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    char     d_name[1];
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_DUPFD_CLOEXEC 1030

#define PROT_NONE    0
#define PROT_READ    1
#define PROT_WRITE   2
#define PROT_EXEC    4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#ifndef MAP_FAILED
#define MAP_FAILED    ((void*)-1)
#endif

uint32_t getpid(void);
int32_t  write(int32_t fd, const void* buf, uint32_t count);
int32_t  read(int32_t fd, void* buf, uint32_t count);
void     putchar(char c);
void     clear(void);
int32_t  fork(void);
int32_t  open(const char* pathname, uint8_t flag);
int32_t  close(int32_t fd);
int32_t  lseek(int32_t fd, int32_t offset, uint8_t whence);
int32_t  unlink(const char* pathname);
int32_t  mkdir(const char* pathname);
int32_t  rmdir(const char* pathname);
int32_t  chdir(const char* path);
char*    getcwd(char* buf, uint32_t size);
int32_t  stat(const char* path, struct FS_STAT* buf);
struct FS_DIR* opendir(const char* name);
int32_t  closedir(struct FS_DIR* dir);
struct FS_DIRENT* readdir(struct FS_DIR* dir);
void     rewinddir(struct FS_DIR* dir);
void     ps(void);
int32_t  execv(const char* path, const char* argv[]);
__attribute__((noreturn)) void exit(int32_t status);
int32_t  wait(int32_t* status);
int32_t  pipe(int32_t pipefd[2]);
void     fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd);
int32_t  gui_start(void);
void*    brk(void* addr);
void*    sbrk(intptr_t inc);
void*    mmap(void* addr, uint32_t len, int prot, int flags, int fd, uint32_t offset);
void*    mmap2(void* addr, uint32_t len, int prot, int flags, int fd, uint32_t offset);
int32_t  munmap(void* addr, uint32_t len);
int32_t  mprotect(void* addr, uint32_t len, int prot);
int32_t  futex(uint32_t uaddr, int op, uint32_t val, void* timeout);
int32_t  clone(int (*fn)(void*), void* child_stack, uint32_t flags, void* arg);

int32_t  fstat(int32_t fd, struct FS_STAT* buf);
int32_t  dup(int32_t oldfd);
int32_t  dup2(int32_t oldfd, int32_t newfd);
int32_t  fcntl(int32_t fd, int32_t cmd, uint32_t arg);
int32_t  getdents(int32_t fd, struct LINUX_DIRENT* dirp, uint32_t count);
int32_t  readlink(const char* path, char* buf, uint32_t bufsiz);
int32_t  access(const char* path, int32_t mode);
int32_t  rename(const char* oldpath, const char* newpath);
int32_t  truncate(const char* path, int32_t length);
int32_t  chmod(const char* path, uint32_t mode);
int32_t  clock_gettime(int32_t clk_id, struct SYS_TIMESPEC* tp);
int32_t  gettimeofday(struct SYS_TIMEVAL* tv, void* tz);
int32_t  nanosleep(const struct SYS_TIMESPEC* req, struct SYS_TIMESPEC* rem);
uint32_t getuid(void);
uint32_t getgid(void);
uint32_t geteuid(void);
uint32_t getegid(void);
void     exit_group(int32_t status);

int32_t  icmp_send(uint32_t dst, uint16_t id, uint16_t seq);
int32_t  icmp_recv(struct NET_PING_REPLY* buf, int32_t max);

int32_t  socket(int32_t domain, int32_t type, int32_t proto);
int32_t  sock_bind(int32_t fd, uint32_t ip, uint16_t port);
int32_t  sock_listen(int32_t fd, int32_t backlog);
int32_t  sock_connect(int32_t fd, uint32_t ip, uint16_t port);
int32_t  sock_send(int32_t fd, const void* buf, uint32_t len);
int32_t  sock_recv(int32_t fd, void* buf, uint32_t len);
int32_t  sock_sendto(int32_t fd, const void* buf, uint32_t len, uint32_t daddr,
                     uint16_t dport);
int32_t  sock_recvfrom(int32_t fd, void* buf, uint32_t len, uint32_t* saddr,
                       uint16_t* sport);
int32_t  sock_accept(int32_t fd);
int32_t  sock_close(int32_t fd);
int32_t  sock_shutdown(int32_t fd, int32_t how);
int32_t  sock_getsockname(int32_t fd, uint32_t *ip, uint16_t *port);
int32_t  sock_getpeername(int32_t fd, uint32_t *ip, uint16_t *port);
int32_t  sock_getsockopt(int32_t fd, int32_t level, int32_t optname, void* val,
                         uint32_t* len);
int32_t  sock_setsockopt(int32_t fd, int32_t level, int32_t optname,
                         const void* val, uint32_t len);
int32_t  sock_select(int32_t nfds, uint32_t* rfds, uint32_t* wfds,
                     uint32_t* efds, int32_t timeout_ms);

#define NET_FDSET_WORDS 2
typedef uint32_t net_fd_set[NET_FDSET_WORDS];
#define FD_ZERO(s) do { uint32_t* _b = (uint32_t*)(s); _b[0] = 0; _b[1] = 0; } while (0)
#define FD_SET(fd, s) (((uint32_t*)(s))[(fd) / 32] |= 1u << ((fd) % 32))
#define FD_CLR(fd, s) (((uint32_t*)(s))[(fd) / 32] &= ~(1u << ((fd) % 32)))
#define FD_ISSET(fd, s) ((((uint32_t*)(s))[(fd) / 32] >> ((fd) % 32)) & 1u)

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

#define CLONE_VM       0x00000100
#define CLONE_FS       0x00000200
#define CLONE_FILES    0x00000400
#define CLONE_SIGHAND  0x00000800
#define CLONE_THREAD   0x00010000
#define CLONE_SETTLS   0x00080000

#endif
