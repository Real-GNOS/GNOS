#ifndef SHELL_H
#define SHELL_H

#include "kernel/fs/fs.h"

void print_prompt(void);
void my_shell(void *arg);

extern char final_path[MAX_PATH_LEN];

#endif
