#include "libc/compat/lc.h"

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    lc_puts("\n[CoreZ compat shim] started\n");

    long pid = lc_getpid();
    lc_puts("  getpid() = ");
    lc_putuint((uint32_t)pid);
    lc_puts("  (errno = ");
    lc_putuint((uint32_t)errno);
    lc_puts(")\n");

    lc_puts("  TLS via gs:0  ->  errno slot @ offset 0\n");
    lc_puts("  six-register ABI: lc_mmap(0,4096,3,0x22,7,0x99) = ");
    long m = lc_mmap((void *)0, 4096, 3, 0x22, 7, 0x99);
    lc_puthex((uint32_t)m);
    lc_puts("  (errno = ");
    lc_putuint((uint32_t)errno);
    lc_puts(")\n");

    lc_puts("  writev([\"con-\",\"cat\",\"-enated\"]) -> ");
    struct LC_IOVEC iov[3];
    iov[0].base = (uint32_t)"con-";
    iov[0].len = 4;
    iov[1].base = (uint32_t)"cat";
    iov[1].len = 3;
    iov[2].base = (uint32_t)"-enated";
    iov[2].len = 7;
    long nv = lc_writev(1, iov, 3);
    lc_puts("  (scatter count = ");
    lc_putuint((uint32_t)nv);
    lc_puts(")\n");

    lc_puts("  open(\"/no/such/file\", 0) = ");
    long fd = lc_open("/no/such/file", O_RDONLY);
    lc_puthex((uint32_t)fd);
    lc_puts("  -> errno = ");
    lc_putuint((uint32_t)errno);
    lc_puts("  (non-zero: errno round-trip OK)\n");

    lc_puts("auxv:\n");
    lc_puts("  AT_PAGESZ = ");
    lc_putuint((uint32_t)lc_auxv_get(AT_PAGESZ));
    lc_puts("\n");
    lc_puts("  AT_CLKTCK = ");
    lc_putuint((uint32_t)lc_auxv_get(AT_CLKTCK));
    lc_puts("\n");
    lc_puts("  AT_ENTRY  = ");
    lc_puthex((uint32_t)lc_auxv_get(AT_ENTRY));
    lc_puts("\n");

    lc_puts("[compat shim] done\n");
    return 0;
}
