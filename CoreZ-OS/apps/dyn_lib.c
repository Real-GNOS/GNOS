#include <string.h>

static const char LIB_DYN_TAG[] = "libdyndemo.so";

int dyn_lib_answer(void) {
    return 42;
}

const char *dyn_lib_tag(void) {
    return LIB_DYN_TAG;
}

int dyn_lib_strlen(const char *s) {
    return (int)strlen(s);
}
