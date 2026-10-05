bits 64
section .text

extern kmain
extern __bss_start
extern __bss_end
extern _kernel_phys_end

global entry_start
entry_start:

        mov     r8d, eax
        mov     r9, rbx
        mov     r10d, edx
        mov     r11, cr3

        mov     rax, qword [abs 0x98008]       
        shr     rax, 21
        shl     rax, 21
        test    rax, rax
        jz      .have_base
        mov     r10d, eax
.have_base:
        test    r10d, r10d
        jz      .remapped

        movabs  rax, _kernel_phys_end
        add     rax, 0x1FFFFF
        shr     rax, 21                        
        cmp     eax, 1
        jbe     .remapped
        mov     rcx, 0x98000                   
        mov     edx, r10d
        mov     rsi, 1
.remap:
        cmp     rsi, rax
        jae     .remapped
        mov     rdi, rsi
        dec     rdi
        shl     rdi, 21
        add     rdi, rdx
        or      rdi, 0x83
        mov     qword [rcx + rsi*8], rdi
        inc     rsi
        jmp     .remap
.remapped:
        mov     cr3, r11

        mov     dx, 0x3F8 + 1
        xor     al, al
        out     dx, al
        inc     dx
        out     dx, al
        inc     dx
        out     dx, al
        inc     dx
        mov     al, 0x03
        out     dx, al
        inc     dx
        mov     al, 0x00
        out     dx, al
        inc     dx
        out     dx, al
        inc     dx
        out     dx, al

        mov     dx, 0x3F8
        mov     al, 'K'
        out     dx, al
        mov     al, '!'
        out     dx, al
        mov     al, 0x0A
        out     dx, al

        movabs  rdi, __bss_start
        movabs  rax, __bss_end
        sub     rax, rdi
        mov     rcx, rax
        xor     eax, eax
        cld
        rep     stosb

        cli
        mov     edi, r8d
        mov     esi, r9d
        mov     edx, r10d
        call    kmain
.spin:  hlt
        jmp     .spin