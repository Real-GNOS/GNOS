#ifndef SYSCALL_FILE_SYSCALL_H
#define SYSCALL_FILE_SYSCALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t sys_fstat(int32_t fd, void *buf);
int32_t sys_dup(int32_t oldfd);
int32_t sys_dup_from(int32_t oldfd, uint32_t min_local);
int32_t sys_dup2(int32_t oldfd, int32_t newfd);
int32_t sys_fcntl(int32_t fd, int32_t cmd, uint32_t arg);

int32_t sys_getdents(int32_t fd, void *dirp, uint32_t count);
int32_t sys_readlink(const char *path, char *buf, uint32_t bufsiz);
int32_t sys_access(const char *path, int32_t mode);
int32_t sys_rename(const char *oldpath, const char *newpath);
int32_t sys_truncate(const char *path, int32_t length);
int32_t sys_chmod(const char *path, uint32_t mode);

#ifdef __cplusplus
}

#endif

#endif
