ENTRY(_start)

PHDRS
{
    phdr    PT_PHDR PHDRS;
    interp  PT_INTERP;
    text    PT_LOAD FILEHDR PHDRS FLAGS(5);
    data    PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC;
}

SECTIONS
{
    . = SIZEOF_HEADERS;

    .interp : { *(.interp) } :text :interp

    .dynsym   : { *(.dynsym) } :text
    .dynstr   : { *(.dynstr) } :text
    .hash     : { *(.hash) } :text
    .gnu.hash : { *(.gnu.hash) } :text
    .gnu.version   : { *(.gnu.version) } :text
    .gnu.version_d : { *(.gnu.version_d) } :text
    .gnu.version_r : { *(.gnu.version_r) } :text

    .text : {
        *(.text.unlikely)
        *(.text.startup)
        *(.text)
        *(.text.*)
        *(.plt)
        *(.plt.*)
        *(.init)
        *(.fini)
    } :text

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :text

    .rela.dyn  : { *(.rela.dyn) } :text
    .rela.plt  : { *(.rela.plt) } :text
    .rela.got  : { *(.rela.got) } :text
    .relr.dyn  : { *(.relr.dyn) } :text
    .rel.dyn   : { *(.rel.dyn) } :text
    .rel.plt   : { *(.rel.plt) } :text
    .rel.got   : { *(.rel.got) } :text

    . = ALIGN(0x1000);

    .data : {
        *(.data)
        *(.data.*)
        *(.tdata)
        *(.got)
        *(.got.*)
        *(.init_array*)
        *(.fini_array*)
    } :data

    .dynamic : { *(.dynamic) } :data :dynamic

    .bss : {
        *(.bss)
        *(.bss.*)
        *(.tbss)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame*)
    }
}
