#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)"(\\w+)@(\\w+)",
                                   PCRE2_ZERO_TERMINATED, 0, &errcode,
                                   &erroffset, NULL);
    if (re == NULL) {
        write(2, "pcre2: compile FAIL\n", 20);
        _exit(2);
    }
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    PCRE2_SPTR subj = (PCRE2_SPTR)"light@corez";
    int rc = pcre2_match(re, subj, PCRE2_ZERO_TERMINATED, 0, 0, md, NULL);
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
    char buf[80];
    int ok = (rc == 3);
    int n = snprintf(buf, sizeof buf,
                     "pcre2: rc=%d user=%.*s host=%.*s -> %s\n", rc,
                     ok ? (int)(ov[3] - ov[2]) : 0, subj + ov[2],
                     ok ? (int)(ov[5] - ov[4]) : 0, subj + ov[4],
                     ok ? "PASS" : "FAIL");
    write(1, buf, (size_t)n);
    _exit(ok ? 0 : 1);
}
