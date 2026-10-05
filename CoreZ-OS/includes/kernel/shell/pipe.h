#ifndef SHELL_PIPE_H
#define SHELL_PIPE_H

#include <stdint.h>

#define PIPE_FLAG 0xFFFF
#define PIPE_RD_FLAG 0xFFFD

uint32_t fd_local2global(uint32_t local_fd);

int32_t is_pipe(uint32_t local_fd);
int32_t pipe_is_read_end(uint32_t local_fd);
int32_t pipe_end_alive(uint32_t global_fd);
uint32_t pipe_buffered(uint32_t local_fd);
int32_t sys_pipe(int32_t pipefd[2]);
uint32_t pipe_read(int32_t fd, void *buf, uint32_t count);
uint32_t pipe_write(int32_t fd, const void *buf, uint32_t count);
void sys_fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd);

#endif
