#ifndef SHELL_BUILDIN_CMD_H
#define SHELL_BUILDIN_CMD_H

#include <stdint.h>

void make_clear_abs_path(char *path, char *wash_buf);
void buildin_ls(int32_t argc, char **argv);
char *buildin_cd(int32_t argc, char **argv);
int32_t buildin_mkdir(int32_t argc, char **argv);
int32_t buildin_rmdir(int32_t argc, char **argv);
int32_t buildin_rm(int32_t argc, char **argv);
void buildin_pwd(int32_t argc, char **argv);
void buildin_ps(int32_t argc, char **argv);
void buildin_clear(int32_t argc, char **argv);
void buildin_shutdown(int32_t argc, char **argv);
void buildin_smash(int32_t argc, char **argv);

#endif
