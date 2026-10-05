bits 64
section .text

global asm_mwait_supported
asm_mwait_supported:
        push    rbx
        mov     eax, 1
        cpuid
        shr     ecx, 3
        and     ecx, 1
        mov     eax, ecx
        pop     rbx
        ret

global asm_sti_mwait
asm_sti_mwait:
        sti
        mov     rax, rdi
        xor     ecx, ecx
        xor     edx, edx
        monitor
        mov     eax, 1
        xor     ecx, ecx
        mwait
        ret
