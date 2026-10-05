#include "lib/str/str.h"

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) {
        p++;
    }
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != '\0') {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n && a[i] && a[i] == b[i]; i++) {
    }
    if (i == n) {
        return 0;
    }
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) {
        d++;
    }
    while ((*d++ = *src++) != '\0') {
    }
    return dst;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }
    return (ch == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;
    while (*s) {
        if (*s == ch) {
            last = s;
        }
        s++;
    }
    if (ch == '\0') {
        return (char *)s;
    }
    return (char *)last;
}

char *u32_to_dec(uint32_t v, char *buf) {
    char tmp[10];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = 0;
    return buf + n;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n && ((uintptr_t)d & 7)) {
        *d++ = *s++;
        n--;
    }
    uint64_t *dq = (uint64_t *)d;
    const uint64_t *sq = (const uint64_t *)s;
    while (n >= 8) {
        *dq++ = *sq++;
        n -= 8;
    }
    d = (uint8_t *)dq;
    s = (const uint8_t *)sq;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        return memcpy(dst, src, n);
    }
    if (d > s) {
        d += n;
        s += n;
        while (n && ((uintptr_t)d & 7)) {
            *--d = *--s;
            n--;
        }
        uint64_t *dq = (uint64_t *)d;
        const uint64_t *sq = (const uint64_t *)s;
        while (n >= 8) {
            *--dq = *--sq;
            n -= 8;
        }
        d = (uint8_t *)dq;
        s = (const uint8_t *)sq;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    uint64_t w = (uint64_t)(uint8_t)c * 0x0101010101010101ULL;
    while (n && ((uintptr_t)p & 7)) {
        *p++ = (uint8_t)c;
        n--;
    }
    uint64_t *q = (uint64_t *)p;
    while (n >= 8) {
        *q++ = w;
        n -= 8;
    }
    p = (uint8_t *)q;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}
