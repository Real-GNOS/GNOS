bits 64

section .text code align=16

global efi_entry
extern efi_main

efi_entry:
        sub     rsp, 40
        call    efi_main
        add     rsp, 40
        ret

global efi_rdtsc

efi_rdtsc:
        rdtsc
        shl     rdx, 32
        or      rax, rdx
        ret

section .landing code align=16

global efi_landing_start
global efi_landing_end

efi_landing_start:
        mov     r10, rcx
        mov     r8,  qword [r10 + 0]
        mov     r9,  qword [r10 + 8]
        mov     r11, qword [r10 + 16]
        mov     ebx, dword [r10 + 24]
        mov     edx, dword [r10 + 28]
        mov     edi, dword [r10 + 32]
        mov     cr3, r8
        mov     rsp, r11
        mov     eax, edi
        jmp     r9

efi_landing_end: