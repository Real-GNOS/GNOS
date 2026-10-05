OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(entry_start)

_kernel_phys_start = 0x00280000;
_kernel_virt_base  = 0xC0000000;

SECTIONS
{
    . = _kernel_virt_base + _kernel_phys_start;

    _kernel_virt_start = .;

    .mb2 ALIGN(16) : AT(ADDR(.mb2) - _kernel_virt_base)
    {
        KEEP(*(.mb2))
    }

    .text ALIGN(16) : AT(ADDR(.text) - _kernel_virt_base)
    {
        *(.text)
        *(.text.*)
        *(.ltext)
        *(.ltext.*)
    }

    .rodata ALIGN(4) : AT(ADDR(.rodata) - _kernel_virt_base)
    {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
    }

    .drivers ALIGN(8) : AT(ADDR(.drivers) - _kernel_virt_base)
    {
        __drivers_start = .;
        KEEP(*(.drivers))
        __drivers_end = .;
    }

    .data ALIGN(4) : AT(ADDR(.data) - _kernel_virt_base)
    {
        *(.data)
        *(.data.*)
        *(.ldata)
        *(.ldata.*)
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - _kernel_virt_base)
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.lbss)
        *(.lbss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    }

    .kernel_orphans ALIGN(16) : AT(ADDR(.kernel_orphans) - _kernel_virt_base)
    {
        *(.gnu.linkonce.*)
        *(.tm_clone_table)
        *(.data.rel.ro)
        *(.data.rel.ro.*)
        *(.got)
        *(.got.plt)
    }
    _kernel_virt_end = .;
    _kernel_phys_end = . - _kernel_virt_base;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
