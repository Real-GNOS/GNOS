#ifndef USER_STDIO_H
#define USER_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

uint32_t vsprintf(char *str, const char *format, va_list ap);

int vsnprintf(char *str, size_t n, const char *format, va_list ap);

void printf(const char *format, ...);

uint32_t sprintf(char *buf, const char *format, ...);

#endif
