bits 64
section .text

global inb
inb:
    mov     dx, di
    in      al, dx
    ret

global outb
outb:
    mov     dx, di
    mov     al, sil
    out     dx, al
    ret

global insw
insw:
    mov     ecx, edx          
    mov     dx, di
    mov     rdi, rsi
    cld
    rep     insw
    ret

global outsw
outsw:
    mov     ecx, edx   
    mov     dx, di
    cld
    rep     outsw
    ret

global inw
inw:
    mov     dx, di
    in      ax, dx
    ret

global outw
outw:
    mov     dx, di
    mov     ax, si
    out     dx, ax
    ret

global inl
inl:
    mov     dx, di
    in      eax, dx
    ret

global outl
outl:
    mov     dx, di
    mov     eax, esi
    out     dx, eax
    ret