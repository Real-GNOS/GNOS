#include <stdio.h>

int main(int argc, char **argv) {
    int i;
    printf("win_main: argc=%d\n", argc);
    for (i = 0; i < argc; i++)
        printf("argv[%d]=%s\n", i, argv[i]);
    printf("sum: %d %s %d\n", 20, "+", 26);
    return 0;
}