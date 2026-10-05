bits 32

MB2_MAGIC           equ 0xE85250D6
MB2_ARCH_I386       equ 0
MB2_BOOT_MAGIC      equ 0x36D76289

MB2_HDR_END         equ 0
MB2_HDR_INFO_REQ    equ 1
MB2_HDR_ENTRY_ADDR  equ 3
MB2_HDR_FRAMEBUFFER equ 5

MB2_TAG_FRAMEBUFFER equ 8

MB2_REQ_MEMINFO     equ 4
MB2_REQ_MMAP        equ 6
MB2_REQ_FRAMEBUFFER equ 8
MB2_REQ_ACPI_OLD    equ 14
MB2_REQ_ACPI_NEW    equ 15
MB2_REQ_COUNT       equ 5

PTE_TABLE_RW        equ 0x07
PTE_KERN_RW         equ 0x83
PAGE_2M             equ 0x200000
PD_ENTRIES          equ 512
VRAM_2M_MASK        equ 0xFFE00000
VRAM_PAGES          equ 8

PML4_PHYS           equ 0x90000
PDPT_PHYS           equ 0x91000
PD_LO_PHYS          equ 0x92000
PD_VRAM_PHYS        equ 0x94000
PD_MMIO_PHYS        equ 0x96000
PD_HI_PHYS          equ 0x98000
PAGE_TABLE_BYTES    equ PD_HI_PHYS - PML4_PHYS + 0x1000
STACK_TOP           equ 0x90000

KPHYS_NONE          equ 0x200000
IOAPIC_PHYS         equ 0xFEC00000
LAPIC_PHYS          equ 0xFEE00000

extern entry_start
extern _kernel_phys_start

%define PHYS(label) (_kernel_phys_start + ((label) - mb2_mode_dispatch))

section .mb2 progbits alloc exec nowrite align=16

global mb2_mode_dispatch

mb2_mode_dispatch:
        mov     ecx, cr0
        test    ecx, 0x80000000
        jz      mb2_from_32bit
        jmp     entry_start                    

mb2_from_32bit:
        cli
        mov     ebp, ebx                      
        mov     esp, STACK_TOP                 

        xor     eax, eax
        mov     edi, PML4_PHYS
        mov     ecx, PAGE_TABLE_BYTES / 4
        cld
        rep     stosd                      

        mov     dword [PML4_PHYS], PDPT_PHYS | PTE_TABLE_RW

        mov     dword [PDPT_PHYS + 0 * 8], PD_LO_PHYS | PTE_TABLE_RW
        mov     dword [PDPT_PHYS + 1 * 8], PD_MMIO_PHYS | PTE_TABLE_RW
        mov     dword [PDPT_PHYS + 2 * 8], PD_VRAM_PHYS | PTE_TABLE_RW
        mov     dword [PDPT_PHYS + 3 * 8], PD_HI_PHYS | PTE_TABLE_RW

        mov     edi, PD_LO_PHYS
        mov     esi, PD_HI_PHYS
        xor     ecx, ecx
        xor     eax, eax
.identity:
        mov     [edi + ecx * 8], eax
        or      dword [edi + ecx * 8], PTE_KERN_RW
        mov     [esi + ecx * 8], eax
        or      dword [esi + ecx * 8], PTE_KERN_RW
        add     eax, PAGE_2M
        inc     ecx
        cmp     ecx, PD_ENTRIES
        jb      .identity

        mov     dword [PD_MMIO_PHYS + 0 * 8], IOAPIC_PHYS | PTE_KERN_RW
        mov     dword [PD_MMIO_PHYS + 1 * 8], LAPIC_PHYS | PTE_KERN_RW

        call    mb2_map_vram

        lgdt    [PHYS(mb2_gdt_desc)]

        mov     eax, cr4
        or      eax, 1 << 5                   
        mov     cr4, eax
        mov     eax, PML4_PHYS
        mov     cr3, eax
        mov     ecx, 0xC0000080              
        rdmsr
        or      eax, 1 << 8                    
        wrmsr
        mov     eax, cr0
        or      eax, 0x80000001                
        mov     cr0, eax
        jmp     0x08:PHYS(mb2_long_mode)

mb2_map_vram:
        mov     esi, ebp
        test    esi, esi
        jz      .done
        mov     ecx, [esi]                 
        add     ecx, esi
        add     esi, 8
.loop:
        cmp     esi, ecx
        jae     .done
        mov     eax, [esi]                  
        test    eax, eax
        jz      .done
        cmp     eax, MB2_TAG_FRAMEBUFFER
        je      .found
        mov     eax, [esi + 4]              
        add     eax, 7
        and     eax, ~7
        add     esi, eax
        jmp     .loop
.found:
        mov     eax, [esi + 12]              
        test    eax, eax
        jnz     .done                          
        mov     eax, [esi + 8]
        and     eax, VRAM_2M_MASK
        mov     edi, PD_VRAM_PHYS
        mov     ecx, VRAM_PAGES
.fill:
        mov     [edi], eax
        mov     dword [edi + 4], 0
        or      dword [edi], PTE_KERN_RW
        add     eax, PAGE_2M
        add     edi, 8
        dec     ecx
        jnz     .fill
.done:
        ret

bits 64

mb2_long_mode:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     rsp, STACK_TOP
        mov     r9, entry_start
        mov     ebx, ebp                       
        mov     edx, KPHYS_NONE
        mov     eax, MB2_BOOT_MAGIC
        jmp     r9

align 8
mb2_gdt:
        dq      0
        dq      0x00AF9A000000FFFF             
        dq      0x00CF92000000FFFF             
mb2_gdt_desc:
        dw      mb2_gdt_desc - mb2_gdt - 1
        dd      PHYS(mb2_gdt)

align 8
mb2_header:
        dd      MB2_MAGIC
        dd      MB2_ARCH_I386
        dd      mb2_header_end - mb2_header
        dd      -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))

align 8
        dw      MB2_HDR_INFO_REQ
        dw      0
        dd      8 + 4 * MB2_REQ_COUNT
        dd      MB2_REQ_MEMINFO
        dd      MB2_REQ_MMAP
        dd      MB2_REQ_FRAMEBUFFER
        dd      MB2_REQ_ACPI_OLD
        dd      MB2_REQ_ACPI_NEW

align 8
        dw      MB2_HDR_FRAMEBUFFER
        dw      0
        dd      20
        dd      1024
        dd      768
        dd      32

align 8
        dw      MB2_HDR_ENTRY_ADDR
        dw      0
        dd      12
        dd      _kernel_phys_start

align 8
        dw      MB2_HDR_END
        dw      0
        dd      8
mb2_header_end: