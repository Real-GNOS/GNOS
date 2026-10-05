/* linker.ld — GnuKr.elf: 64-bit PIE kernel (relocated by Limine). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

PHDRS
{
    limine_requests PT_LOAD FLAGS(4);   /* read-only */
    text            PT_LOAD FLAGS(5);   /* rx */
    rodata          PT_LOAD FLAGS(4);   /* r  */
    data            PT_LOAD FLAGS(6);   /* rw */
    dynamic         PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
    . = 0;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : {
        *(.text)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : {
        *(.rodata*)
    } :rodata

    /* The exported-symbol table for loadable modules (EXPORT_SYMBOL in
     * module.h).  Bounded symbols are defined here so the module loader
     * does not depend on ld's __start_/__stop_ auto-symbols. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    /* The RELATIVE relocations Limine applies -- and kaslr.c re-applies
     * after moving the kernel to a random base.  Bracketed symbols because
     * the relocator must walk exactly this table. */
    .rela.dyn : {
        *(.rela.dyn)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __ksymtab : {
        __start___ksymtab = .;
        KEEP(*(__ksymtab))
        __stop___ksymtab = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } :rodata :dynamic

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : {
        *(.data)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .bss : {
        _bss_start = .;
        *(COMMON)
        *(.bss)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    _kernel_end = .;
}

/* The dynamic relocations for a -static-pie kernel are synthesized by the
 * linker, not gathered from input sections, so boundary symbols cannot be
 * defined inside the output section -- they would see it empty.  ADDR and
 * SIZEOF are evaluated against the final placement instead. */
__rela_start = ADDR(.rela.dyn);
__rela_end   = __rela_start + SIZEOF(.rela.dyn);
