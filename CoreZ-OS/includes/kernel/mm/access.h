#ifndef MEMORY_ACCESS_H
#define MEMORY_ACCESS_H

#include <stddef.h>
#include <stdint.h>

#define USER_SPACE_END 0xc0000000u

int access_ok(const void *addr, size_t n, int write);
int user_range_readable(uint32_t addr, uint32_t len);
int user_range_writable(uint32_t addr, uint32_t len);
int copy_str_from_user(char *dst, const char *src, uint32_t max);
int copy_str_from_user_len(char *dst, const char *src, uint32_t max);
int copy_from_user(void *dst, const void *src, uint32_t len);
int copy_to_user(void *udst, const void *src, uint32_t len);
int user_strnlen(const char *src, uint32_t max);

int page_is_mapped(uint32_t v);

#endif
