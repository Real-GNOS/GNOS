/* SPDX-License-Identifier: GPL-2.0 */
/*
 * case2handler.c — expand a C `switch (nr)` dispatch into one entry-point
 * function per case label. (GPLv2)
 *
 * HOST tool, never compiled into the kernel:
 *   cc -std=gnu11 -O2 -Wall -Wextra -o tools/case2handler tools/case2handler.c
 *
 *   case2handler <input.c> <outdir> [-p <label-prefix>] [-n <handler-prefix>]
 *                [-s <switch-marker>] [-u <user-names-file>]
 *
 * Emits into <outdir>:
 *   syscall_list.h        __SYSCALL(nr, fn) lines; the file is included twice
 *                         with two different __SYSCALL definitions (once for
 *                         prototypes, once for sys_call_table[]), so it has
 *                         NO include guard.
 *   handlers_syscall.inc  __x64_sys_* definitions for the general set
 *   handlers_sysuser.inc  __x64_sys_* definitions for the user/permission set
 *
 * How a case group becomes a handler:
 *   - Consecutive top-level `case SYS_x:` labels with no statements between
 *     them form ONE group and share ONE handler (named after the first label).
 *     The generated prologue exposes `nr = r->rax`, so bodies that branch on
 *     the dispatcher's local `nr` (chown/lchown, renameat/renameat2) keep
 *     working unchanged.
 *   - The case body is emitted verbatim inside `do { ... } while (0)`.  That
 *     preserves `break` semantics: a case-level `break` leaves the do-while,
 *     while a `break` inside a nested switch still only leaves that switch.
 *   - `a1/a2/a3`, `p`, `nr` and `ret` are declared in the prologue; `r->r10`,
 *     `r->r8`, `r->r9` and `r` itself remain available.  No textual
 *     substitution is needed in the body -- this is the point: there is zero
 *     risk of dropping an `(int)` cast or swapping an argument.
 *   - A bare `return;` becomes `return SYS_FRAME_REPLACED;` (execve success /
 *     rt_sigreturn replace the whole register frame, so the dispatcher must
 *     not write RAX or run its syscall-exit tail).
 *   - The top-level `default:` group is skipped; it becomes the dispatcher's
 *     out-of-range / -ENOSYS branch.
 *
 * The user/permission set defaults to the list below; -u replaces it with a
 * file containing one SYS_* name per line (blank lines and # comments ok).
 * A group is routed to the user output when ANY of its labels is in that set.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ utils */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("case2handler: out of memory\n");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("case2handler: out of memory\n");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} strvec_t;

static void sv_push(strvec_t *s, char *p)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->v   = xrealloc(s->v, s->cap * sizeof(*s->v));
    }
    s->v[s->n++] = p;
}

static int sv_has(const strvec_t *s, const char *needle)
{
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->v[i], needle) == 0)
            return 1;
    return 0;
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("case2handler: cannot open %s\n", path);
    if (fseek(f, 0, SEEK_END) != 0)
        die("case2handler: cannot seek %s\n", path);
    long sz = ftell(f);
    if (sz < 0)
        die("case2handler: cannot size %s\n", path);
    rewind(f);
    char *buf = xmalloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        die("case2handler: short read on %s\n", path);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Split a NUL-terminated buffer into lines, in place (newlines -> NUL). */
static strvec_t split_lines(char *buf)
{
    strvec_t s = {0};
    char    *p = buf;
    while (*p) {
        char *start = p;
        while (*p && *p != '\n')
            p++;
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        size_t l = strlen(start);
        if (l && start[l - 1] == '\r')
            start[l - 1] = '\0';
        sv_push(&s, start);
    }
    return s;
}

/* Net open-minus-close braces for one line, skipping comments and
 * string/char literals.  `*in_comment` carries block-comment state
 * across lines. */
static int line_brace_delta(const char *line, int *in_comment)
{
    int         delta = 0;
    const char *p     = line;
    while (*p) {
        if (*in_comment) {
            if (p[0] == '*' && p[1] == '/') {
                *in_comment = 0;
                p += 2;
            } else {
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            *in_comment = 1;
            p += 2;
            continue;
        }
        if (p[0] == '/' && p[1] == '/')
            break;
        if (p[0] == '"' || p[0] == '\'') {
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1])
                    p += 2;
                else
                    p++;
            }
            if (*p)
                p++;
            continue;
        }
        if (*p == '{')
            delta++;
        else if (*p == '}')
            delta--;
        p++;
    }
    return delta;
}

/* -------------------------------------------------------------- case model */

typedef struct {
    strvec_t labels;
    strvec_t body;
    strvec_t lead; /* comment block that precedes this case in the source */
    int      is_default;
} group_t;

static void group_init(group_t *g)
{
    memset(g, 0, sizeof(*g));
}

/* A top-level label is exactly four leading spaces then `case ` or
 * `default:`.  Deeper-indented labels (nested switches) are body text.
 * Returns 1 for `case`, -1 for `default`, 0 for an ordinary body line.
 * `rest` receives everything after the colon (e.g. " {" or ""). */
static int top_label(const char *line, char *tok, size_t tokcap, const char **rest)
{
    if (strncmp(line, "    ", 4) != 0)
        return 0;
    const char *p = line + 4;
    if (p[0] == ' ' || p[0] == '\t')
        return 0;
    if (strncmp(p, "default:", 8) == 0) {
        *rest = p + 8;
        return -1;
    }
    if (strncmp(p, "case ", 5) != 0)
        return 0;
    const char *q      = p + 5;
    const char *colon  = strchr(q, ':');
    if (!colon)
        return 0;
    size_t len = (size_t)(colon - q);
    if (len == 0 || len >= tokcap)
        return 0;
    memcpy(tok, q, len);
    tok[len] = '\0';
    *rest    = colon + 1;
    return 1;
}

/* Turn the text after a label's colon into a standalone body line.  This is
 * how `case SYS_mkdir: {` keeps its opening brace. */
static char *rest_line(const char *rest)
{
    while (*rest == ' ' || *rest == '\t')
        rest++;
    if (*rest == '\0')
        return NULL;
    size_t n = strlen(rest);
    char  *s = xmalloc(4 + n + 1);
    memcpy(s, "    ", 4);
    memcpy(s + 4, rest, n + 1);
    return s;
}

/* Classify a line for trailing-comment extraction: 0 = code, 1 = comment,
 * 2 = blank.  Block-comment start/continuation/end lines and line comments
 * count as comments; a pointer dereference such as `*(int *)p = 0;` is
 * code and must not be mistaken for one. */
static int line_class(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return 2;
    if (p[0] == '/' && (p[1] == '*' || p[1] == '/'))
        return 1;
    if (p[0] == '*' && (p[1] == ' ' || p[1] == '\t' || p[1] == '/'))
        return 1;
    return 0;
}

/* Strip up to four leading spaces: inter-case comments sit one level deep
 * inside the switch and become top-level comments before the next handler. */
static char *dedent4(const char *line)
{
    int n = 0;
    while (n < 4 && (line[n] == ' ' || line[n] == '\t'))
        n++;
    return xstrdup(line + n);
}

/* Comments between two cases describe the *following* syscall.  Pull the
 * trailing comment/blank run off `g->body` and hand its comment lines back
 * so they can be emitted above the next handler instead of dangling at the
 * bottom of this one. */
static strvec_t pull_trailing_comments(group_t *g)
{
    strvec_t lead = {0};
    if (!g)
        return lead;
    size_t end = g->body.n;
    while (end > 0 && line_class(g->body.v[end - 1]) != 0)
        end--;
    for (size_t i = end; i < g->body.n; i++)
        if (line_class(g->body.v[i]) == 1)
            sv_push(&lead, dedent4(g->body.v[i]));
    g->body.n = end;
    return lead;
}

/* ----------------------------------------------------------------- default
 * credential + permission syscalls (the sysuser.c split).  Override with -u. */
static const char *const k_default_user[] = {
    "SYS_getuid",     "SYS_geteuid",    "SYS_getgid",      "SYS_getegid",
    "SYS_umask",      "SYS_chmod",      "SYS_fchmod",      "SYS_fchmodat",
    "SYS_chown",      "SYS_lchown",     "SYS_fchown",      "SYS_fchownat",
    "SYS_access",     "SYS_faccessat",  "SYS_faccessat2",
    "SYS_setuid",     "SYS_setgid",     "SYS_setreuid",    "SYS_setregid",
    "SYS_setresuid",  "SYS_setresgid",  "SYS_getgroups",   "SYS_setgroups",
    "SYS_getresuid",  "SYS_getresgid",  NULL,
};

static strvec_t load_user_names(const char *path)
{
    strvec_t s = {0};
    if (path) {
        char *buf = read_all(path);
        for (char *p = strtok(buf, "\r\n"); p; p = strtok(NULL, "\r\n")) {
            while (*p == ' ' || *p == '\t')
                p++;
            char *e = p + strlen(p);
            while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = '\0';
            if (*p && *p != '#')
                sv_push(&s, xstrdup(p));
        }
        free(buf);
    } else {
        for (const char *const *q = k_default_user; *q; q++)
            sv_push(&s, xstrdup(*q));
    }
    return s;
}

/* ----------------------------------------------------------------- emission */

static void emit_body_line(FILE *f, const char *line)
{
    if (*line == '\0') {
        fputc('\n', f);
        return;
    }
    fputs("    ", f); /* one extra level, inside do { */
    const char *p = line;
    const char *hit;
    while ((hit = strstr(p, "return;")) != NULL) {
        fwrite(p, 1, (size_t)(hit - p), f);
        fputs("return SYS_FRAME_REPLACED;", f);
        p = hit + strlen("return;");
    }
    fputs(p, f);
    fputc('\n', f);
}

static char *handler_name(const char *label, const char *label_prefix,
                          const char *handler_prefix)
{
    size_t lp = strlen(label_prefix);
    if (strncmp(label, label_prefix, lp) != 0)
        die("case2handler: label '%s' does not start with '%s'\n", label, label_prefix);
    const char *tail = label + lp;
    size_t      n    = strlen(handler_prefix) + strlen(tail) + 1;
    char       *s    = xmalloc(n);
    snprintf(s, n, "%s%s", handler_prefix, tail);
    return s;
}

static void emit_handler(FILE *f, const group_t *g, const char *hname)
{
    for (size_t i = 0; i < g->lead.n; i++)
        fprintf(f, "%s\n", g->lead.v[i]);
    fprintf(f, "int64_t %s(regs_t *r)\n{\n", hname);
    fputs("    uint64_t nr  __attribute__((unused)) = r->rax;\n", f);
    fputs("    uint64_t a1  __attribute__((unused)) = r->rdi;\n", f);
    fputs("    uint64_t a2  __attribute__((unused)) = r->rsi;\n", f);
    fputs("    uint64_t a3  __attribute__((unused)) = r->rdx;\n", f);
    fputs("    proc_t  *p   __attribute__((unused)) = proc_current();\n", f);
    fputs("    int64_t  ret = 0;\n", f);
    fputs("    do {\n", f);
    for (size_t i = 0; i < g->body.n; i++)
        emit_body_line(f, g->body.v[i]);
    fputs("    } while (0);\n", f);
    fputs("    return ret;\n", f);
    fputs("}\n\n", f);
}

/* ---------------------------------------------------------------------- main */

static void usage(void)
{
    die("usage: case2handler <input.c> <outdir> [-p <label-prefix>] "
        "[-n <handler-prefix>] [-s <switch-marker>] [-u <user-names-file>]\n");
}

int main(int argc, char **argv)
{
    const char *input        = NULL;
    const char *outdir       = NULL;
    const char *label_prefix = "SYS_";
    const char *hprefix      = "__x64_sys_";
    const char *marker       = "switch (nr) {";
    const char *user_file    = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (i + 1 >= argc)
                usage();
            if (strcmp(argv[i], "-p") == 0)
                label_prefix = argv[++i];
            else if (strcmp(argv[i], "-n") == 0)
                hprefix = argv[++i];
            else if (strcmp(argv[i], "-s") == 0)
                marker = argv[++i];
            else if (strcmp(argv[i], "-u") == 0)
                user_file = argv[++i];
            else
                usage();
        } else if (!input) {
            input = argv[i];
        } else if (!outdir) {
            outdir = argv[i];
        } else {
            usage();
        }
    }
    if (!input || !outdir)
        usage();

    strvec_t user = load_user_names(user_file);
    char    *buf  = read_all(input);
    strvec_t ls   = split_lines(buf);

    /* Locate the dispatch switch. */
    long sw = -1;
    for (size_t i = 0; i < ls.n; i++) {
        const char *p = ls.v[i];
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, marker, strlen(marker)) == 0) {
            if (sw >= 0)
                die("case2handler: more than one '%s' in %s\n", marker, input);
            sw = (long)i;
        }
    }
    if (sw < 0)
        die("case2handler: no '%s' found in %s\n", marker, input);

    /* Find its closing brace by brace matching (skips comments/strings). */
    long sw_end   = -1;
    int  depth    = 0;
    int  in_cmt   = 0;
    for (size_t i = (size_t)sw; i < ls.n; i++) {
        depth += line_brace_delta(ls.v[i], &in_cmt);
        if (i > (size_t)sw && depth == 0) {
            sw_end = (long)i;
            break;
        }
    }
    if (sw_end < 0)
        die("case2handler: unterminated switch at line %ld\n", sw + 1);

    /* Split the switch body into case groups. */
    group_t *groups   = NULL;
    size_t   ngroups  = 0, gcap = 0;
    group_t *cur      = NULL;
    int      just_lbl = 0;
    size_t   nlabels  = 0;
    strvec_t pending_lead = {0};

    for (size_t i = (size_t)sw + 1; i < (size_t)sw_end; i++) {
        char        tok[128];
        const char *rest = NULL;
        int         lt   = top_label(ls.v[i], tok, sizeof(tok), &rest);

        if (lt == 1 || lt == -1) {
            if (!cur || !just_lbl) {
                pending_lead = pull_trailing_comments(cur);
                if (ngroups == gcap) {
                    gcap   = gcap ? gcap * 2 : 64;
                    groups = xrealloc(groups, gcap * sizeof(*groups));
                }
                cur = &groups[ngroups++];
                group_init(cur);
                cur->lead = pending_lead;
            }
            if (lt == -1) {
                cur->is_default = 1;
            } else {
                sv_push(&cur->labels, xstrdup(tok));
                nlabels++;
            }
            char *rl = rest_line(rest);
            if (rl)
                sv_push(&cur->body, rl);
            just_lbl = 1;
        } else {
            if (cur)
                sv_push(&cur->body, xstrdup(ls.v[i]));
            just_lbl = 0;
        }
    }

    /* Open outputs. */
    char path[4096];
    FILE *flist, *fgen, *fusr;

    snprintf(path, sizeof(path), "%s/syscall_list.h", outdir);
    if (!(flist = fopen(path, "w")))
        die("case2handler: cannot write %s\n", path);
    snprintf(path, sizeof(path), "%s/handlers_syscall.inc", outdir);
    if (!(fgen = fopen(path, "w")))
        die("case2handler: cannot write %s\n", path);
    snprintf(path, sizeof(path), "%s/handlers_sysuser.inc", outdir);
    if (!(fusr = fopen(path, "w")))
        die("case2handler: cannot write %s\n", path);

    fputs("/* SPDX-License-Identifier: GPL-2.0 */\n"
          "/* Generated by tools/case2handler.c -- DO NOT EDIT.\n"
          " * Included twice with different __SYSCALL definitions: once to\n"
          " * declare handlers, once to fill sys_call_table[].  No guard. */\n"
          "#ifndef __SYSCALL\n"
          "#error \"define __SYSCALL(nr, fn) before including syscall_list.h\"\n"
          "#endif\n",
          flist);
    fputs("/* Generated by tools/case2handler.c -- append to syscall.c. */\n\n", fgen);
    fputs("/* Generated by tools/case2handler.c -- append to sysuser.c. */\n\n", fusr);

    size_t n_handlers = 0, n_gen = 0, n_usr = 0, n_default = 0;
    for (size_t i = 0; i < ngroups; i++) {
        group_t *g = &groups[i];
        if (g->is_default) {
            n_default++;
            continue;
        }
        if (g->labels.n == 0)
            die("case2handler: internal error: empty label group\n");

        char *hname = handler_name(g->labels.v[0], label_prefix, hprefix);

        int is_user = 0;
        for (size_t k = 0; k < g->labels.n; k++)
            if (sv_has(&user, g->labels.v[k]))
                is_user = 1;

        for (size_t k = 0; k < g->labels.n; k++)
            fprintf(flist, "__SYSCALL(%s, %s)\n", g->labels.v[k], hname);

        emit_handler(is_user ? fusr : fgen, g, hname);
        n_handlers++;
        if (is_user)
            n_usr++;
        else
            n_gen++;
    }

    fclose(flist);
    fclose(fgen);
    fclose(fusr);

    /* Sanity: every configured user name should have appeared. */
    for (size_t i = 0; i < user.n; i++) {
        int found = 0;
        for (size_t k = 0; k < ngroups && !found; k++)
            for (size_t j = 0; j < groups[k].labels.n; j++)
                if (strcmp(groups[k].labels.v[j], user.v[i]) == 0) {
                    found = 1;
                    break;
                }
        if (!found)
            fprintf(stderr, "case2handler: WARNING: user name '%s' not in input\n",
                    user.v[i]);
    }

    fprintf(stderr,
            "case2handler: switch lines %ld..%ld, %zu groups, %zu labels, "
            "%zu handlers (%zu general + %zu user), %zu default\n",
            sw + 1, sw_end + 1, ngroups, nlabels, n_handlers, n_gen, n_usr,
            n_default);
    return 0;
}
