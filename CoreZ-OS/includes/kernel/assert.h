#ifndef ASSERT_H
#define ASSERT_H

#ifdef NDEBUG
#define ASSERT(expr) ((void)0)
#else
#define ASSERT(expr) ((expr) ? (void)0 : assert_fail(#expr, __FILE__, __LINE__))
#endif

void assert_fail(const char *expr, const char *file, int line)
    __attribute__((noreturn));

#endif
