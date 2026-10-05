ORG     0x0600

%define PART_START      2048
%define RESERVED_SECT   2
%define FAT_SECTORS     127
%define FAT_COUNT       2
%define SPC             1
%define ROOT_CLUSTER    2

FAT_LBA   equ PART_START + RESERVED_SECT
DATA_LBA  equ FAT_LBA + FAT_COUNT*FAT_SECTORS
LOAD_ADDR equ 0xC200

DIR_BUF equ 0x0800
FAT_BUF equ 0x0A00

        jmp     short start
        nop

        db      "COREZ   "
        dw      512
        db      SPC
        dw      RESERVED_SECT
        db      FAT_COUNT
        dw      0
        dw      0
        db      0xF8
        dw      0
        dw      18
        dw      2
        dd      PART_START
        dd      16384
        dd      FAT_SECTORS
        dw      0
        dw      0
        dd      ROOT_CLUSTER
        dw      1
        dw      0
        times 12 db 0
        db      0x80
        db      0
        db      0x29
        dd      0x12345678
        db      "COREZ     "
        db      "FAT32   "

start:
        cli
        cld
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        sti
        mov     al, byte [0x0FF0]
        mov     byte [drive], al

        mov     word [loadcur], LOAD_ADDR

        mov     word [curclu], ROOT_CLUSTER
.dirscan:
        mov     ax, word [curclu]
        sub     ax, 2
        add     ax, DATA_LBA
        mov     word [bufaddr], DIR_BUF
        call    readsec
        jc      fail
        mov     bx, DIR_BUF
        mov     di, 16
.entry:
        cmp     byte [bx], 0x00
        je      .nextroot
        cmp     byte [bx], 0xE5
        je      .next
        push    si
        push    bx
        push    di
        mov     si, fname
        mov     di, bx
        mov     cx, 11
        repe    cmpsb
        pop     di
        pop     bx
        pop     si
        je      .found
.next:
        add     bx, 32
        dec     di
        jnz     .entry

.nextroot:
        call    nextclu
        jc      fail
        jmp     .dirscan

.found:
        mov     ax, word [bx+26]
        mov     word [curclu], ax

.loadloop:
        mov     ax, word [curclu]
        sub     ax, 2
        mov     dx, SPC
        mul     dx
        add     ax, DATA_LBA
        mov     word [dap+0x08], ax
        mov     word [dap+0x0C], 0
        mov     ax, word [loadcur]
        mov     word [bufaddr], ax
        mov     word [dap+0x04], ax
        mov     word [dap+0x06], 0
        mov     word [dap+0x02], SPC    
        mov     si, dap
        mov     dl, byte [drive]
        mov     ah, 0x42
        int     0x13
        jc      fail
        mov     ax, SPC
        shl     ax, 9
        add     word [loadcur], ax

        call    nextclu
        jc      .doneload
        jmp     .loadloop
.doneload:
        jmp     0x0000:LOAD_ADDR

nextclu:
        mov     ax, word [curclu]
        shl     ax, 2
        mov     word [fatbyte], ax
        mov     ax, word [fatbyte]
        shr     ax, 9
        add     ax, FAT_LBA
        mov     word [bufaddr], FAT_BUF
        call    readsec
        jc      fail
        mov     ax, word [fatbyte]
        and     ax, 0x1FF
        mov     di, ax
        mov     eax, dword [FAT_BUF + di]
        and     eax, 0x0FFFFFFF
        mov     word [curclu], ax
        cmp     eax, 0x0FFFFFF8
        jae     .end
        clc
        ret
.end:
        stc
        ret

readsec:
        push    si
        mov     word [dap+0x02], 1
        mov     si, ax
        mov     ax, word [bufaddr]
        mov     word [dap+0x04], ax
        mov     word [dap+0x06], 0
        mov     word [dap+0x08], si
        mov     word [dap+0x0C], 0
        mov     cx, 5
.try:
        mov     si, dap
        mov     dl, byte [drive]
        mov     ah, 0x42
        int     0x13
        jnc     .chk
        jmp     .fail
.chk:
        test    ah, ah
        jz      .done
.fail:
        xor     ah, ah
        int     0x13
        loop    .try
        stc
        pop     si
        ret
.done:
        clc
        pop     si
        ret

fail:
        mov     si, msg
.put:
        lodsb
        test    al, al
        jz      .h
        mov     ah, 0x0E
        mov     bx, 15
        int     0x10
        jmp     .put
.h:     hlt
        jmp     .h

msg:    db      0x0D, 0x0A, "vbr: load err", 0

fname:  db      "LOADER  BIN"

        align   8
dap:    db      0x10
        db      0
        dw      0
        dw      0
        dw      0
        dd      0
        dd      0

bufaddr: dw     0
loadcur: dw     0
curclu:  dw     0
fatbyte: dw     0
drive:   db     0

        times   510-($-$$) db 0
        dw      0xAA55