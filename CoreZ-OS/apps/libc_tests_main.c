#include <stdio.h>

#define RUN_TEST(a)                                                            \
    {                                                                          \
        extern int test_##a(void);                                             \
        int e = test_##a();                                                    \
        if (e)                                                                 \
            printf("%s test failed, %d error(s)\n", #a, e);                    \
        else                                                                   \
            printf("%s test passed\n", #a);                                    \
        err += e;                                                              \
    }

int main(void) {
    int err = 0;

    RUN_TEST(string);
    RUN_TEST(qsort);
    RUN_TEST(strtol);
    RUN_TEST(strtod);
    RUN_TEST(basename);
    RUN_TEST(dirname);
    RUN_TEST(fnmatch);

    printf("\ntotal errors: %d\n", err);
    return !!err;
}
