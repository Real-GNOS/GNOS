/* mmtest.c - TEMPORARY diagnostic: compare file-backed mappings byte for
 * byte against pread() of the same file, both for the whole-file mapping and
 * for musl-style per-segment MAP_FIXED mappings.  Any mismatch means the
 * kernel's mmap copy-in is handing the loader wrong bytes. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCRATCH 0x600000000000ULL

static int errs;

static void cmp_range(int fd, unsigned char *map, unsigned long foff,
                      unsigned long len, const char *tag)
{
    static unsigned char ref[65536];
    unsigned long o;
    int shown = 0;

    for (o = 0; o < len; o += sizeof(ref)) {
        unsigned long n = len - o < sizeof(ref) ? len - o : sizeof(ref);
        long got = pread(fd, ref, n, foff + o);
        if (got != (long)n) {
            printf("  %s: pread(%lu+%lu) = %ld (want %lu) errno=%d\n",
                   tag, foff + o, n, got, n, errno);
            errs++;
            return;
        }
        for (unsigned long k = 0; k < n; k++) {
            if (map[o + k] != ref[k]) {
                printf("  %s: MISMATCH at file off %lu: file=%02x map=%02x\n",
                       tag, foff + o + k, ref[k], map[o + k]);
                errs++;
                if (++shown >= 4)
                    return;
            }
        }
        if (shown >= 4)
            return;
    }
    printf("  %s: %lu bytes identical\n", tag, len);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/lib/libc.so";
    int fd = open(path, O_RDONLY);
    struct stat st;

    if (fd < 0) {
        printf("MMTEST: cannot open %s (errno %d)\n", path, errno);
        return 1;
    }
    fstat(fd, &st);
    unsigned long fsize = st.st_size;
    printf("MMTEST: %s size=%lu\n", path, fsize);

    /* --- 1. whole-file mapping, the way mapping #1 is made --- */
    unsigned char *whole = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (whole == MAP_FAILED) {
        printf("  whole-file mmap failed (errno %d)\n", errno);
        errs++;
    } else {
        cmp_range(fd, whole, 0, fsize, "whole-file map");
        munmap(whole, fsize);
    }

    /* --- 2. musl-style per-segment MAP_FIXED mappings --- */
    unsigned char hdr[4096];
    pread(fd, hdr, sizeof(hdr), 0);
    unsigned short phnum     = *(unsigned short *)(hdr + 0x38);
    unsigned long  phoff     = *(unsigned long *)(hdr + 0x20);
    unsigned short phentsize = *(unsigned short *)(hdr + 0x36);

    unsigned char phbuf[8192];
    if (phnum * phentsize > sizeof(phbuf)) phnum = sizeof(phbuf) / phentsize;
    pread(fd, phbuf, phnum * phentsize, phoff);

    unsigned long scratch = SCRATCH;
    for (int i = 0; i < phnum; i++) {
        unsigned char *ph = phbuf + i * phentsize;
        if (ph[0] != 1) continue; /* PT_LOAD */
        unsigned long vaddr  = *(unsigned long *)(ph + 0x10);
        unsigned long memsz  = *(unsigned long *)(ph + 0x28);
        unsigned long filesz = *(unsigned long *)(ph + 0x20);
        unsigned long off    = *(unsigned long *)(ph + 0x08);
        unsigned int  flg    = *(unsigned int *)(ph + 0x04);

        int prot = PROT_READ;
        if (flg & 2) prot |= PROT_WRITE;
        if (flg & 1) prot |= PROT_EXEC;

        unsigned long va_pg  = vaddr & ~0xFFFUL;
        unsigned long end_pg = (vaddr + memsz + 0xFFFUL) & ~0xFFFUL;
        unsigned long len    = end_pg - va_pg;
        unsigned long foff   = off & ~0xFFFUL;

        void *m = mmap((void *)scratch + va_pg, len, prot,
                       MAP_FIXED | MAP_PRIVATE, fd, foff);
        if (m == MAP_FAILED) {
            printf("  seg%d: mmap(va=%lx len=%lu foff=%lx) failed (errno %d)\n",
                   i, va_pg, len, foff, errno);
            errs++;
            continue;
        }
        printf("  seg%d: mapped va=%lx len=%lu foff=%lx prot=%x\n",
               i, va_pg, len, foff, prot);

        /* the file-backed part: [va_pg, va_pg+filesz) shows file bytes from
         * foff; the rest is zero (bss) */
        cmp_range(fd, (unsigned char *)m + (va_pg & 0xFFF), foff,
                  filesz + (va_pg & 0xFFF), "seg file part");
        if (memsz > filesz) {
            unsigned long bz0 = (va_pg & 0xFFF) + filesz;
            unsigned char *p = (unsigned char *)m + bz0;
            unsigned long n = memsz - filesz;
            unsigned long bad = 0;
            for (unsigned long k = 0; k < n; k++)
                if (p[k]) bad++;
            if (bad)
                printf("  seg%d: bss region has %lu nonzero bytes\n", i, bad);
            else
                printf("  seg%d: bss %lu bytes all zero\n", i, n);
        }

        scratch += len + 0x1000000UL;
    }

    printf("MMTEST DONE errs=%d\n", errs);
    return errs ? 1 : 0;
}
