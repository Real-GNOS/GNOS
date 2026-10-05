#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dyn_lib_answer(void);
const char *dyn_lib_tag(void);
int dyn_lib_strlen(const char *s);

static const char TEXT[] = "dynamic";

int main(void) {
    void *heap = malloc(64);

    printf("DYNAMIC_TAG=%s\n", dyn_lib_tag());
    printf("DYNAMIC_ANSWER=%d\n", dyn_lib_answer());
    printf("DYNAMIC_STRLEN=%d\n", dyn_lib_strlen(TEXT));
    printf("DYNAMIC_LIBC_MALLOC=%s\n", heap != NULL ? "ok" : "fail");
    printf("DYNAMIC_LIBC_STRLEN=%d\n", (int)strlen(TEXT));

    free(heap);
    return 0;
}
