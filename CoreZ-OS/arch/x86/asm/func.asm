bits 64
section .text

global asm_hlt
asm_hlt: hlt
        ret

global asm_xchg
asm_xchg:
        mov     eax, esi
        xchg    eax, [rdi]
        ret

global asm_pause
asm_pause:
        pause
        ret

global asm_cli
asm_cli: cli
        ret

global asm_sti
asm_sti: sti
        ret

global asm_stihlt
asm_stihlt: sti
            hlt
            ret

global asm_read_cr0
asm_read_cr0:
        mov     rax, cr0
        ret

global asm_write_cr0
asm_write_cr0:
        mov     cr0, rdi
        ret

global asm_read_cr2
asm_read_cr2:
        mov     rax, cr2
        ret

global asm_read_cr4
asm_read_cr4:
        mov     rax, cr4
        ret

global asm_write_cr4
asm_write_cr4:
        mov     cr4, rdi
        ret

global asm_write_cr3
asm_write_cr3:
        mov     cr3, rdi
        ret

global asm_read_cr3
asm_read_cr3:
        mov     rax, cr3
        ret

global asm_rdmsr
asm_rdmsr:
        mov     ecx, edi
        rdmsr
        shl     rdx, 32
        or      rax, rdx
        ret

global asm_wrmsr
asm_wrmsr:
        mov     ecx, edi
        mov     eax, esi
        shr     rsi, 32
        mov     edx, esi
        wrmsr
        ret

global asm_save_eflags
asm_save_eflags:
        pushfq
        pop     rax
        ret

global asm_restore_eflags
asm_restore_eflags:
        push    rdi
        popfq
        ret

global asm_lgdt
asm_lgdt:
        lgdt    [rdi]
        ret

global asm_reload_segments
asm_reload_segments:
        push    qword 0x08
        lea     rax, [rel .reload]
        push    rax
        retfq
.reload:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        ret

global asm_ltr
asm_ltr:
        mov     ax, di
        ltr     ax
        ret

global asm_str
asm_str:
        str     ax
        ret

global detect_64bit
detect_64bit:                  
        mov     eax, 1
        ret

global kernel_thread_entry
kernel_thread_entry:
        mov     rdi, r15
        mov     rsi, r14
        xor     ebp, ebp       
        extern  kernel_thread_entry_c
        call    kernel_thread_entry_c

.endless:
        cli
        hlt
        jmp     .endless