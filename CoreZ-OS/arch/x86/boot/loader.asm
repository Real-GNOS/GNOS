KERNEL  equ     0x00280000
KERNEL_VIRT equ 0xC0000000 + KERNEL
KASLR_MIN  equ    0x00800000
KASLR_SLOTS equ   124
KERNEL_FIXED equ  0x00200000

%if KERNEL_FIXED & 0x1FFFFF
        %error "KERNEL_FIXED must be 2MB aligned: setup_hpd maps it as a 2MB page"
%endif
%if KASLR_MIN & 0x1FFFFF
        %error "KASLR_MIN must be 2MB aligned"
%endif

MENU_ITEMS   equ  2
MENU_TIMEOUT equ  5
PREF_W       equ  1024
PREF_H       equ  768
TICKS_PER_SEC equ 18     
KEY_UP       equ  0x48
KEY_DOWN     equ  0x50
KEY_ENTER    equ  0x1C
KEY_1        equ  0x02
KEY_2        equ  0x03
FONT_BASE    equ  0x9C000
MENU_BG      equ  0x000B0F1A
MENU_NORMAL  equ  0x00D7DCE5
MENU_HI      equ  0x00FFB454
MENU_TITLE   equ  0x00FFFFFF
DSKCAC  equ     0x00100000
DSKCAC0 equ     0x00008000
VBEMODE equ     0x143
VBEINFO equ     0x8000
VBE_MODEINFO equ 0x9000
VBE_LFB_VIRT equ 0x80000000
VBE_LFB_PAGES equ 8

CYLS    equ     0x0FF0
LEDS    equ     0x0FF1
VMODE   equ     0x0FF2
SCRNX   equ     0x0FF4
SCRNY   equ     0x0FF6
VRAM    equ     0x0FF8
VRAMBYTES equ  0x0FFC

STACK_PHYS equ  0x00090000
MBI        equ  0x00005000

%define PART_LBA        2048
%define RESERVED_SECT   2
%define FAT_SECTORS     127
%define FAT_COUNT       2
%define SPC             1
%define ROOT_CLUSTER    2
FAT_LBA   equ PART_LBA + RESERVED_SECT
DATA_LBA  equ FAT_LBA + FAT_COUNT*FAT_SECTORS
DIR_BUF   equ 0x3000
FAT_BUF   equ 0x3400
BOUNCE    equ 0x4000
E820_MAX  equ 100
STAGE_HI  equ 0x00100000
MAX_KSIZE equ 0x00180000

        org     0xC200

        jmp     start

start:
        cld
        mov     al, byte [0x0FF0]
        mov     byte [l_drive], al

        mov     dword [l_bestmode], 0xFFFF
        mov     dword [l_bestarea], 0
        mov     dword [l_exact], 0xFFFF
        xor     ax, ax
        mov     es, ax
        mov     di, VBEINFO
        mov     dword [es:di], 'VBE2'
        mov     ax, 0x4F00
        int     0x10
        cmp     ax, 0x004F
        jne     vbe_fallback

        mov     si, [es:VBEINFO+14]
        mov     ax, [es:VBEINFO+16]
        mov     fs, ax
        mov     bx, 0
.vbe_scan:
        mov     ax, [fs:si]
        cmp     ax, 0xFFFF
        je      .vbe_scan_done
        push    si
        push    fs
        push    bx
        push    ax
        mov     cx, ax
        mov     ax, VBE_MODEINFO
        mov     es, ax
        xor     di, di
        mov     ax, 0x4F01
        int     0x10
        pop     dx
        pop     bx
        pop     fs
        pop     si
        cmp     ax, 0x004F
        jne     .vbe_next
        mov     ax, VBE_MODEINFO
        mov     es, ax
        mov     ax, [es:0x0000]
        test    ax, 0x0010
        jz      .vbe_next
        test    ax, 0x0080
        jz      .vbe_next
        cmp     byte [es:0x0019], 32
        jne     .vbe_next
        cmp     word [es:0x0012], PREF_W
        jne     .vbe_area
        cmp     word [es:0x0014], PREF_H
        jne     .vbe_area
        mov     [l_exact], dx
        jmp     .vbe_scan_done
.vbe_area:
        movzx   eax, word [es:0x0012]
        movzx   ecx, word [es:0x0014]
        imul    eax, ecx
        cmp     eax, [l_bestarea]
        jbe     .vbe_next
        mov     [l_bestarea], eax
        movzx   eax, dx
        mov     [l_bestmode], eax
.vbe_next:
        add     si, 2
        inc     bx
        cmp     bx, 1024
        jb      .vbe_scan
.vbe_scan_done:
        mov     ax, [l_exact]
        cmp     ax, 0xFFFF
        jne     .vbe_have
        mov     ax, [l_bestmode]
        cmp     ax, 0xFFFF
        jne     .vbe_have
        jmp     vbe_fallback
.vbe_have:
        mov     [l_mode], ax
        mov     bx, ax
        or      bx, 0x4000
        mov     ax, 0x4F02
        int     0x10
        cmp     ax, 0x004F
        jne     vbe_fallback
        mov     cx, [l_mode]
        mov     ax, VBE_MODEINFO
        mov     es, ax
        xor     di, di
        mov     ax, 0x4F01
        int     0x10
        cmp     ax, 0x004F
        jne     vbe_fallback
        jmp     vbe_configure
vbe_fallback:
        mov     word [l_fbptr], fb_modes
.fb_loop:
        mov     si, [l_fbptr]
        mov     ax, [si]
        cmp     ax, 0xFFFF
        je      .fb_8bpp
        mov     [l_mode], ax
        mov     cx, ax
        mov     ax, VBE_MODEINFO
        mov     es, ax
        xor     di, di
        mov     ax, 0x4F01
        int     0x10
        cmp     ax, 0x004F
        jne     .fb_next
        mov     ax, VBE_MODEINFO
        mov     es, ax
        mov     ax, [es:0x0000]
        test    ax, 0x0080
        jz      .fb_next
        cmp     byte [es:0x0019], 32
        jne     .fb_next
        mov     cx, [l_mode]
        mov     bx, cx
        or      bx, 0x4000
        mov     ax, 0x4F02
        int     0x10
        cmp     ax, 0x004F
        jne     .fb_next
        mov     cx, [l_mode]
        mov     ax, VBE_MODEINFO
        mov     es, ax
        xor     di, di
        mov     ax, 0x4F01
        int     0x10
        cmp     ax, 0x004F
        jne     .fb_next
        jmp     vbe_configure
.fb_next:
        add     word [l_fbptr], 2
        jmp     .fb_loop
.fb_8bpp:
        mov     al, 0x13
        mov     ah, 0x00
        int     0x10
        mov     byte [VMODE], 8
        mov     word [SCRNX], 320
        mov     word [SCRNY], 200
        mov     dword [VRAM], 0x000A0000
        mov     dword [VRAMBYTES], 64000
        mov     dword [vram_pitch], 320
        mov     byte [bpp_div8], 1
        mov     byte [fg_rpos], 0
        mov     byte [fg_rsize], 0
        mov     byte [fg_gpos], 0
        mov     byte [fg_gsize], 0
        mov     byte [fg_bpos], 0
        mov     byte [fg_bsize], 0
        jmp     vbe_done
vbe_configure:
        movzx   eax, word [es:0x0012]
        mov     [SCRNX], ax
        movzx   eax, word [es:0x0014]
        mov     [SCRNY], ax
        mov     eax, [es:0x0028]
        mov     [VRAM], eax
        movzx   eax, word [es:0x0010]
        test    eax, eax
        jnz     .pitch_ok
        movzx   eax, word [es:0x0012]
        shl     eax, 2
.pitch_ok:
        mov     [vram_pitch], eax
        mov     al, [es:0x0019]
        mov     [VMODE], al
        shr     al, 3
        jnz     .bpp_ok
        mov     al, 1
.bpp_ok:
        mov     [bpp_div8], al
        mov     al, [es:0x0020]
        mov     [fg_rpos], al
        mov     al, [es:0x001F]
        mov     [fg_rsize], al
        mov     al, [es:0x0022]
        mov     [fg_gpos], al
        mov     al, [es:0x0021]
        mov     [fg_gsize], al
        mov     al, [es:0x0024]
        mov     [fg_bpos], al
        mov     al, [es:0x0023]
        mov     [fg_bsize], al
        mov     eax, [vram_pitch]
        movzx   ecx, word [SCRNY]
        imul    eax, ecx
        mov     [VRAMBYTES], eax

vbe_done:
        cmp     byte [VMODE], 8
        jne     .skip_pal
        call    set_palette
.skip_pal:
        mov     ax, 0x1130
        mov     bh, 0x06
        int     0x10

        push    ds
        push    es
        push    si
        push    di
        push    cx

        mov     ax, es
        mov     si, bp

        mov     bx, 0x9C00
        mov     es, bx
        xor     di, di

        mov     ds, ax

        mov     cx, 256 * 16
        rep     movsb

        pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds

        mov     ah, 0x02
        int     0x16
        mov     [LEDS], al

        xor     ax, ax
        mov     es, ax
        xor     ebx, ebx
        mov     dword [0x6000], 0
        mov     edi, 0x6004

.e820_loop:
        mov     eax, 0xE820
        mov     edx, 0x534D4150
        mov     ecx, 24
        int     0x15
        jc      .e820_done
        cmp     eax, 0x534D4150
        jne     .e820_done
        cmp     ecx, 24
        jae     .e820_norm
        mov     dword [edi+20], 0
.e820_norm:
        add     edi, 24
        inc     dword [0x6000]
        cmp     dword [0x6000], E820_MAX
        jae     .e820_done
        cmp     ebx, 0
        je      .e820_done
        jmp     .e820_loop

.e820_done:
        mov     al, 0xFF
        out     0x21, al
        nop
        out     0xA1, al

        mov     byte [l_rsdp_ok], 0
        mov     byte [l_rsdp_rev], 0
        mov     ax, [0x040E]
        test    ax, ax
        jz      .rsdp_hi
        mov     es, ax
        xor     di, di
        mov     cx, 1024 / 16
        call    .rsdp_range
        jc      .rsdp_hit
.rsdp_hi:
        mov     ax, 0xE000
        mov     es, ax
        xor     di, di
        mov     cx, 0x20000 / 16
        call    .rsdp_range
        jc      .rsdp_hit
        jmp     .rsdp_fin
.rsdp_range:
        push    si
        push    di
        push    cx
        push    bx
.rsdp_r_loop:
        cmp     dword [es:di], 0x20445352
        jne     .rsdp_r_next
        cmp     dword [es:di + 4], 0x20525450
        jne     .rsdp_r_next
        push    si
        mov     si, di
        mov     bx, cx
        mov     cx, 20
        xor     al, al
.rsdp_r_sum:
        add     al, [es:si]
        inc     si
        dec     cx
        jnz     .rsdp_r_sum
        mov     cx, bx
        pop     si
        test    al, al
        jz      .rsdp_r_found
.rsdp_r_next:
        add     di, 16
        dec     cx
        jnz     .rsdp_r_loop
        pop     bx
        pop     cx
        pop     di
        pop     si
        clc
        ret
.rsdp_r_found:
        pop     bx
        pop     cx
        pop     di
        pop     si
        stc
        ret
.rsdp_hit:
        mov     byte [l_rsdp_ok], 1
        push    es
        push    fs
        push    di
        mov     ax, es
        mov     fs, ax
        mov     si, di
        mov     di, l_rsdp_buf
        mov     cx, 20
.rsdp_cp1:
        mov     al, [fs:si]
        mov     [di], al
        inc     si
        inc     di
        dec     cx
        jnz     .rsdp_cp1
        mov     al, [l_rsdp_buf + 15]
        mov     [l_rsdp_rev], al
        cmp     al, 2
        jb      .rsdp_cp_done
        mov     cx, 16
.rsdp_cp2:
        mov     al, [fs:si]
        mov     [di], al
        inc     si
        inc     di
        dec     cx
        jnz     .rsdp_cp2
.rsdp_cp_done:
        pop     di
        pop     fs
        pop     es
.rsdp_fin:
        call    waitkbdout
        mov     al, 0xD1
        out     0x64, al
        call    waitkbdout
        mov     al, 0xDF
        out     0x60, al
        call    waitkbdout

        cli
        lgdt    [GDTR0]
        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax
        mov     bx, 0x08
        mov     gs, bx
        and     eax, 0xFFFFFFFE
        mov     cr0, eax
        sti

        mov     dword [l_loadcur], STAGE_HI

        xor     ax, ax
        mov     es, ax
        mov     word [l_rowcl], ROOT_CLUSTER
.l_dirscan:
        mov     ax, word [l_rowcl]
        sub     ax, 2
        add     ax, DATA_LBA
        mov     word [l_buf], DIR_BUF
        call    l_readsec
        jc      .lkerr
        mov     bx, DIR_BUF
        mov     di, 16
.l_entry:
        cmp     byte [bx], 0x00
        je      .lkerr
        cmp     byte [bx], 0xE5
        je      .l_enext
        push    si
        push    bx
        push    di
        mov     si, l_kname
        mov     di, bx
        mov     cx, 11
        repe    cmpsb
        pop     di
        pop     bx
        pop     si
        je      .l_found
.l_enext:
        add     bx, 32
        dec     di
        jnz     .l_entry
        jmp     .lkerr

.l_found:
        mov     ax, word [bx+26]
        mov     word [l_cluster], ax
        mov     eax, [bx+28]
        mov     [l_ksize], eax
        cmp     eax, MAX_KSIZE
        jbe     .l_loadloop
        mov     si, kmsg2
        jmp     .kerr

.l_loadloop:
        mov     ax, word [l_cluster]
        sub     ax, 2
        mov     dx, SPC
        mul     dx
        add     ax, DATA_LBA
        mov     word [l_lba], ax
        mov     word [l_buf], BOUNCE
        call    l_readsec
        jc      .lkerr
        mov     si, BOUNCE
        mov     edi, [l_loadcur]
        mov     ecx, SPC*512/4
.gcopy:
        mov     eax, [gs:si]
        mov     [gs:edi], eax
        add     si, 4
        add     edi, 4
        dec     ecx
        jnz     .gcopy
        mov     eax, SPC
        shl     eax, 9
        add     dword [l_loadcur], eax

        mov     ax, word [l_cluster]
        shl     ax, 2
        mov     word [l_fbyte], ax
        mov     ax, word [l_fbyte]
        shr     ax, 9
        add     ax, FAT_LBA
        mov     word [l_buf], FAT_BUF
        call    l_readsec
        jc      .lkerr
        mov     ax, word [l_fbyte]
        and     ax, 0x1FF
        mov     di, ax
        mov     eax, dword [FAT_BUF + di]
        and     eax, 0x0FFFFFFF
        mov     word [l_cluster], ax
        cmp     eax, 0x0FFFFFF8
        jae     .l_kload_ok
        jmp     .l_loadloop

.l_kload_ok:
        jmp     kload_ok

.lkerr:
        mov     si, kmsg
.kerr:
        lodsb
        test    al, al
        jz      .khalt
        mov     ah, 0x0E
        mov     bx, 15
        int     0x10
        jmp     .kerr
.khalt:
        hlt
        jmp     .khalt
koverflow:
        bits 32
        cli
.khang:
        hlt
        jmp     .khang
        bits 16
kload_ok:

        cli

        lgdt    [GDTR0]
        lidt    [IDTR0]
        mov     eax, cr0
        and     eax, 0x7FFFFFFF
        or      eax, 0x00000001
        mov     cr0, eax
        jmp     DWORD 2*8:pipelineflush

pipelineflush:
        bits    32

        mov     ax, 1*8
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax

        call    boot_menu

        call    pick_kphys
        mov     eax, [l_kphys]
        add     eax, KERNEL - 0x200000
        mov     edi, eax
        mov     esi, STAGE_HI
        mov     ecx, [l_ksize]
        cmp     ecx, MAX_KSIZE
        ja      koverflow
        add     ecx, 3
        shr     ecx, 2
        call    memcpy

        mov     esp, STACK_PHYS

        mov     edi, 0x90000
        mov     ecx, 0x9000 / 4
        xor     eax, eax
        rep     stosd

        mov     dword [0x90000],     0x91007
        mov     dword [0x91000],     0x92007
        call    setup_hpd
        mov     dword [0x92000 + 0*8], 0x000083
        mov     dword [0x92000 + 1*8], 0x200083
        mov     dword [0x92000 + 2*8], 0x400083
        mov     dword [0x92000 + 3*8], 0x600083
        mov     dword [0x92000 + 4*8], 0x800083
        mov     dword [0x92000 + 5*8], 0xA00083

        mov     dword [0x91000 + 2*8],  0x94007
        mov     eax, [VRAM]
        and     eax, 0xFFE00000
        or      eax, 0x83
        mov     edi, 0x94000
        mov     ecx, VBE_LFB_PAGES
.lfb_map:
        mov     [edi], eax
        mov     dword [edi+4], 0
        add     eax, 0x200000
        add     edi, 8
        dec     ecx
        jnz     .lfb_map

        mov     dword [0x91000 + 1*8],  0x96007
        mov     dword [0x96000 + 0*8],  0xfec00083
        mov     dword [0x96000 + 1*8],  0xfee00083

        mov     edi, MBI
        mov     dword [edi], 0
        mov     dword [edi+4], 0
        add     edi, 8

        mov     ebx, 0x6004
        mov     ecx, [0x6000]
        xor     eax, eax
.memcalc_loop:
        test    ecx, ecx
        jz      .memcalc_done
        cmp     dword [ebx+16], 1
        jne     .memcalc_next
        mov     edx, [ebx]
        add     edx, [ebx+4]
        cmp     edx, eax
        jbe     .memcalc_next
        mov     eax, edx
.memcalc_next:
        add     ebx, 24
        dec     ecx
        jmp     .memcalc_loop
.memcalc_done:
        cmp     eax, 0x100000
        jbe     .memcalc_zero
        sub     eax, 0x100000
        shr     eax, 10
        jmp     .memcalc_store
.memcalc_zero:
        xor     eax, eax
.memcalc_store:
        mov     [l_mem_upper], eax
        mov     edx, 1
        mov     esi, mb_cmdline
        call    mb_str_tag
        mov     edx, 2
        mov     esi, mb_loader_name
        call    mb_str_tag
        mov     dword [edi], 4
        mov     dword [edi+4], 16
        mov     dword [edi+8], 640
        mov     eax, [l_mem_upper]
        mov     dword [edi+12], eax
        add     edi, 16
        call    mb_align
        mov     dword [edi], 5
        mov     dword [edi+4], 20
        movzx   eax, byte [l_drive]
        mov     dword [edi+8], eax
        mov     dword [edi+12], 0xFFFFFFFF
        mov     dword [edi+16], 0xFFFFFFFF
        add     edi, 20
        call    mb_pad8
        mov     ecx, [0x6000]
        test    ecx, ecx
        jz      .mb_skip_mmap
        cmp     ecx, 120
        jbe     .mb_mmap_n
        mov     ecx, 120
.mb_mmap_n:
        push    ecx
        imul    edx, ecx, 24
        lea     eax, [edx+16]
        mov     dword [edi], 6
        mov     dword [edi+4], eax
        mov     dword [edi+8], 24
        mov     dword [edi+12], 0
        add     edi, 16
        pop     ecx
        imul    ecx, ecx, 24
        mov     esi, 0x6004
        rep     movsb
        call    mb_pad8
.mb_skip_mmap:
        mov     dword [edi], 8
        mov     dword [edi+4], 40
        mov     eax, [VRAM]
        mov     dword [edi+8], eax
        mov     dword [edi+12], 0
        mov     eax, [vram_pitch]
        mov     dword [edi+16], eax
        movzx   eax, word [SCRNX]
        mov     dword [edi+20], eax
        movzx   eax, word [SCRNY]
        mov     dword [edi+24], eax
        mov     al, [VMODE]
        mov     byte [edi+28], al
        mov     byte [edi+29], 1
        mov     word [edi+30], 0
        mov     al, [fg_rpos]
        mov     byte [edi+32], al
        mov     al, [fg_rsize]
        mov     byte [edi+33], al
        mov     al, [fg_gpos]
        mov     byte [edi+34], al
        mov     al, [fg_gsize]
        mov     byte [edi+35], al
        mov     al, [fg_bpos]
        mov     byte [edi+36], al
        mov     al, [fg_bsize]
        mov     byte [edi+37], al
        mov     word [edi+38], 0
        add     edi, 40
        call    mb_align
        cmp     byte [l_rsdp_ok], 0
        je      .mb_skip_acpi
        mov     dword [edi], 14
        mov     dword [edi+4], 28
        mov     esi, l_rsdp_buf
        add     edi, 8
        mov     ecx, 20
        rep     movsb
        call    mb_pad8
        cmp     byte [l_rsdp_rev], 2
        jb      .mb_skip_acpi
        mov     dword [edi], 15
        mov     dword [edi+4], 44
        mov     esi, l_rsdp_buf
        add     edi, 8
        mov     ecx, 36
        rep     movsb
        call    mb_pad8
.mb_skip_acpi:

        mov     dword [edi], 0
        mov     dword [edi+4], 8
        add     edi, 8

        mov     eax, edi
        sub     eax, MBI
        mov     dword [MBI], eax
        mov     dword [MBI+4], 0

        lgdt    [GDTR64]

        mov     eax, cr4
        or      eax, 0x20
        mov     cr4, eax

        mov     eax, 0x90000
        mov     cr3, eax

        mov     ecx, 0xC0000080
        rdmsr
        or      eax, 0x100
        wrmsr

        mov     eax, cr0
        or      eax, 0x80000000
        mov     cr0, eax

        jmp     0x08:lg64

        bits    64
lg64:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     rsp, STACK_PHYS

        mov     eax, 0x36D76289
        mov     ebx, MBI
        mov     rcx, KERNEL_VIRT
        mov     edx, [l_kphys]
        jmp     rcx

        bits    16
waitkbdout:
        in      al, 0x64
        and     al, 0x02
        jnz     waitkbdout
        ret

bits    32
memcpy:
        mov     eax, [esi]
        add     esi, 4
        mov     [edi], eax
        add     edi, 4
        sub     ecx, 1
        jnz     memcpy
        ret

mb_align:
        add     edi, 7
        and     edi, 0xFFFFFFF8
        ret
mb_pad8:
        test    edi, 7
        jz      .p8_done
.p8_loop:
        mov     byte [edi], 0
        inc     edi
        test    edi, 7
        jnz     .p8_loop
.p8_done:
        ret
mb_str_tag:
        push    ecx
        push    eax
        mov     ecx, 0
.len:
        cmp     byte [esi+ecx], 0
        je      .len_done
        inc     ecx
        jmp     .len
.len_done:
        lea     eax, [ecx+9]
        add     eax, 7
        and     eax, 0xFFFFFFF8
        mov     [edi], edx
        mov     [edi+4], eax
        add     edi, 8
        inc     ecx
        rep     movsb
        pop     eax
        pop     ecx
        call    mb_pad8
        ret
pick_kphys:
        cmp     dword [l_kaslr], 0
        jne     .do_kaslr
.fixed:
        mov     eax, KERNEL_FIXED
        mov     [l_kphys], eax
        ret
.do_kaslr:
        xor     eax, eax
        mov     ebx, 0x6004
        mov     ecx, [0x6000]
.toploop:
        test    ecx, ecx
        jz      .topdone
        cmp     dword [ebx+16], 1
        jne     .topnext
        mov     edx, [ebx]
        add     edx, [ebx+4]
        cmp     edx, eax
        jbe     .topnext
        mov     eax, edx
.topnext:
        add     ebx, 24
        dec     ecx
        jmp     .toploop
.topdone:
        sub     eax, KASLR_MIN
        jc      .fixed
        shr     eax, 21
        jz      .fixed
        cmp     eax, KASLR_SLOTS
        jbe     .have_slots
        mov     eax, KASLR_SLOTS
.have_slots:
        mov     ecx, eax
        rdtsc
        mov     ebx, eax
        rdtsc
        xor     ebx, eax
        mov     eax, [0x6000]
        xor     ebx, eax
        mov     al, 0
        out     0x70, al
        in      al, 0x71
        movzx   eax, al
        xor     ebx, eax
        imul    ebx, ebx, 1103515245
        add     ebx, 12345
        mov     eax, ebx
        xor     edx, edx
        div     ecx
        mov     eax, edx
        shl     eax, 21
        add     eax, KASLR_MIN
        mov     [l_kphys], eax
        ret

setup_hpd:
        mov     edi, 0x98000
        mov     ecx, 512
        xor     eax, eax
.h:
        mov     edx, eax
        shl     edx, 21
        or      edx, 0x83
        mov     [edi], edx
        mov     dword [edi+4], 0
        add     edi, 8
        inc     eax
        dec     ecx
        jnz     .h
        mov     eax, [l_kphys]
        or      eax, 0x83
        mov     dword [0x98000 + 1*8], eax
        mov     dword [0x98000 + 1*8 + 4], 0
        mov     dword [0x91000 + 3*8], 0x98007
        ret

boot_menu:
        pushad
        call    kbd_flush            
        mov     dword [menu_sel], 0
        mov     dword [menu_secs], MENU_TIMEOUT
        call    menu_clear
        call    menu_draw
        call    get_rtc_sec
        mov     [menu_last_sec], eax
.mloop:
        call    kbd_poll
        test    eax, eax
        jz      .rtc
        mov     dword [menu_secs], 0x7FFFFFFF  
        cmp     eax, KEY_UP
        je      .up
        cmp     eax, KEY_DOWN
        je      .down
        cmp     eax, KEY_1
        je      .num0
        cmp     eax, KEY_2
        je      .num1
        cmp     eax, KEY_ENTER
        je      .done
        jmp     .rtc
.up:
        dec     dword [menu_sel]
        jns     .upcheck
        mov     dword [menu_sel], MENU_ITEMS-1
        jmp     .redraw
.upcheck:
        mov     eax, [menu_sel]
        cmp     eax, MENU_ITEMS
        jb      .redraw
        mov     dword [menu_sel], MENU_ITEMS-1
        jmp     .redraw
.down:
        inc     dword [menu_sel]
        mov     eax, [menu_sel]
        cmp     eax, MENU_ITEMS
        jb      .redraw
        mov     dword [menu_sel], 0
        jmp     .redraw
.num0:
        mov     dword [menu_sel], 0
        jmp     .done
.num1:
        mov     dword [menu_sel], 1
        jmp     .done
.redraw:
        call    menu_draw
        call    menu_draw_count
        jmp     .mloop
.rtc:
        cmp     dword [menu_secs], MENU_TIMEOUT
        ja      .mloop
        call    get_rtc_sec
        cmp     eax, [menu_last_sec]
        je      .mloop
        mov     [menu_last_sec], eax
        dec     dword [menu_secs]
        cmp     dword [menu_secs], 0
        jle     .done
        call    menu_draw_count
        jmp     .mloop
.done:
        cmp     dword [menu_sel], 1
        je      sys_reboot
        mov     dword [l_kaslr], 1       
        popad
        ret

sys_reboot:
.again:
        in      al, 0x64
        test    al, 0x02
        jnz     .again
        mov     al, 0xFE
        out     0x64, al
.h:
        hlt
        jmp     .h

menu_clear:
        pushad
        mov     eax, 0
        mov     ebx, 0
        movzx   ecx, word [SCRNX]
        movzx   edx, word [SCRNY]
        mov     esi, MENU_BG
        call    fill_rect
        popad
        ret

menu_draw:
        pushad
        mov     esi, msg_title
        call    menu_cx
        mov     ebx, 30
        mov     esi, msg_title
        mov     edx, MENU_NORMAL
        call    draw_str
        movzx   eax, word [SCRNY]
        sub     eax, MENU_ITEMS*24
        shr     eax, 1
        mov     [cnt_y], eax
        mov     dword [item_idx], 0
.iloop:
        mov     eax, [item_idx]
        cmp     eax, MENU_ITEMS
        jae     .done
        imul    ecx, eax, 24
        add     ecx, [cnt_y]
        mov     [tmp_y], ecx
        mov     esi, [menu_items + eax*4]
        mov     [tmp_w], esi
        call    menu_cx
        mov     [tmp_x], eax
        mov     eax, [item_idx]
        cmp     eax, [menu_sel]
        jne     .m0
        mov     esi, msg_sel
        mov     edx, MENU_HI
        jmp     .m1
.m0:
        mov     esi, msg_unsel
        mov     edx, MENU_NORMAL
.m1:
        mov     eax, [tmp_x]
        sub     eax, 24
        mov     ebx, [tmp_y]
        call    draw_str
.m2:
        mov     eax, [tmp_x]
        mov     ebx, [tmp_y]
        mov     esi, [tmp_w]
        call    draw_str
        inc     dword [item_idx]
        jmp     .iloop
.done:
        popad
        ret

menu_draw_count:
        pushad
        cmp     dword [menu_secs], MENU_TIMEOUT
        ja      .done
        movzx   eax, word [SCRNY]
        sub     eax, 28
        mov     [cnt_y], eax
        mov     eax, 0
        mov     ebx, [cnt_y]
        movzx   ecx, word [SCRNX]
        mov     edx, 16
        mov     esi, MENU_BG
        call    fill_rect
        mov     esi, msg_count1
        call    strpix8
        mov     [cnt_pix1], eax
        add     eax, 8
        mov     [tmp_w], eax
        mov     esi, msg_count2
        call    strpix8
        add     eax, [tmp_w]
        movzx   ecx, word [SCRNX]
        sub     ecx, eax
        shr     ecx, 1
        mov     [cnt_x], ecx
        mov     eax, [cnt_x]
        mov     ebx, [cnt_y]
        mov     esi, msg_count1
        mov     edx, MENU_NORMAL
        call    draw_str
        mov     eax, [cnt_x]
        add     eax, [cnt_pix1]
        mov     ebx, [cnt_y]
        mov     ecx, [menu_secs]
        add     ecx, '0'
        mov     edx, MENU_HI
        call    draw_char
        mov     eax, [cnt_x]
        add     eax, [cnt_pix1]
        add     eax, 8
        mov     ebx, [cnt_y]
        mov     esi, msg_count2
        mov     edx, MENU_NORMAL
        call    draw_str
.done:
        popad
        ret

menu_cx:
        push    esi
        push    ecx
        push    edx
        xor     ecx, ecx
.cxl:
        lodsb
        test    al, al
        jz      .cxd
        inc     ecx
        jmp     .cxl
.cxd:
        shl     ecx, 3
        movzx   eax, word [SCRNX]
        sub     eax, ecx
        js      .cxz
        shr     eax, 1
        jmp     .cxr
.cxz:
        xor     eax, eax
.cxr:
        pop     edx
        pop     ecx
        pop     esi
        ret

strpix8:
        push    esi
        push    ecx
        xor     ecx, ecx
.spl:
        lodsb
        test    al, al
        jz      .spd
        inc     ecx
        jmp     .spl
.spd:
        shl     ecx, 3
        mov     eax, ecx
        pop     ecx
        pop     esi
        ret

draw_str:
        pushad
        mov     [cur_x], eax
        mov     [cur_y], ebx
.loop:
        lodsb
        test    al, al
        jz      .done
        movzx   ecx, al               
        mov     eax, [cur_x]
        mov     ebx, [cur_y]
        call    draw_char
        add     dword [cur_x], 8
        jmp     .loop
.done:
        popad
        ret

draw_char:
        pushad
        mov     [tmp_x], eax
        mov     [tmp_y], ebx
        mov     [tmp_color], edx
        mov     esi, FONT_BASE
        imul    ecx, ecx, 16
        add     esi, ecx
        movzx   ebx, byte [bpp_div8]
        mov     eax, [tmp_y]
        mul     dword [vram_pitch]
        mov     ecx, [tmp_x]
        imul    ecx, ebx
        add     eax, ecx
        mov     edi, [VRAM]
        add     edi, eax
        mov     ebp, 16
.row:
        movzx   edx, byte [esi]
        mov     ecx, 8
.col:
        shl     dl, 1
        jnc     .adv
        mov     eax, [tmp_color]
        cmp     byte [VMODE], 32
        jne     .b8
        mov     [edi], eax
        jmp     .adv
.b8:
        mov     [edi], al
.adv:
        add     edi, ebx
        dec     ecx
        jnz     .col
        mov     eax, ebx
        shl     eax, 3
        neg     eax
        add     eax, [vram_pitch]
        add     edi, eax
        inc     esi
        dec     ebp
        jnz     .row
        popad
        ret

fill_rect:
        pushad
        mov     [tmp_x], eax
        mov     [tmp_y], ebx
        mov     [tmp_w], ecx
        mov     [tmp_h], edx
        mov     [tmp_color], esi
        mov     eax, ebx
        mul     dword [vram_pitch]
        mov     ecx, [tmp_x]
        movzx   ebx, byte [bpp_div8]
        imul    ecx, ebx
        add     eax, ecx
        mov     edi, [VRAM]
        add     edi, eax
        mov     eax, [tmp_color]
        mov     edx, [tmp_h]
.row:
        push    edi
        mov     ecx, [tmp_w]
        cmp     byte [VMODE], 32
        jne     .b8
        rep     stosd
        jmp     .done
.b8:
        rep     stosb
.done:
        pop     edi
        add     edi, [vram_pitch]
        dec     edx
        jnz     .row
        popad
        ret

kbd_poll:
        in      al, 0x64
        test    al, 0x01
        jz      .none
        in      al, 0x60
        test    al, 0x80
        jnz     .none
        cmp     al, 0xE0
        je      .ext
        movzx   eax, al
        ret
.ext:
        call    kbd_waitdata
        in      al, 0x60
        test    al, 0x80
        jnz     .none
        movzx   eax, al
        ret
.none:
        xor     eax, eax
        ret

kbd_waitdata:
.wait:
        in      al, 0x64
        test    al, 0x01
        jz      .wait
        ret

kbd_flush:
        push    eax
        push    ecx
        mov     ecx, 64
.loop:
        in      al, 0x64
        test    al, 0x01
        jz      .done
        in      al, 0x60
        dec     ecx
        jnz     .loop
.done:
        pop     ecx
        pop     eax
        ret

get_rtc_sec:
.again:
        mov     al, 0x0A
        out     0x70, al
        in      al, 0x71
        test    al, 0x80
        jnz     .again
        mov     al, 0x00
        out     0x70, al
        in      al, 0x71
        movzx   eax, al
        mov     ecx, eax
        and     ecx, 0x0F
        shr     eax, 4
        imul    eax, eax, 10
        add     eax, ecx
        ret

        alignb  16

        align   8
dap:
        db      0x10
        db      0
        dw      0
        dw      0
        dw      0
        dd      0
        dd      0

l_drive: db     0
l_kphys: dd     0
l_ksize: dd     0
l_mem_upper: dd 0
l_rsdp_ok:   db 0
l_rsdp_rev:  db 0
        align 4
l_rsdp_buf:  times 36 db 0
l_loadcur: dd   0
l_rowcl:  dw    0   
l_cluster: dw   0
l_fbyte:  dw    0
l_buf:    dw    0
l_lba:    dw    0
l_retryn: db    0
l_kname:  db    "KERNEL  BIN"

l_bestmode:   dw    0xFFFF
l_exact:      dw    0xFFFF
l_bestarea:   dd    0
l_mode:       dw    0xFFFF
l_fbptr:      dw    0
vram_pitch:   dd    0
bpp_div8:     db    1
fg_rpos:      db    0
fg_rsize:     db    0
fg_gpos:      db    0
fg_gsize:     db    0
fg_bpos:      db    0
fg_bsize:     db    0
fb_modes:     dw    0x143, 0x142, 0x141, 0x144, 0x145, 0x146, 0x147, 0xFFFF
l_kaslr:      dd    1
menu_sel:     dd    0
menu_secs:    dd    0
menu_last_sec: dd  0
item_idx:     dd    0
cnt_y:        dd    0
cnt_x:        dd    0
cnt_pix1:     dd    0
cur_x:        dd    0
cur_y:        dd    0
pitch:        dd    0
tmp_x:        dd    0
tmp_y:        dd    0
tmp_w:        dd    0
tmp_h:        dd    0
tmp_color:    dd    0

msg_title:  db "CoreZ OS Boot Menu", 0
msg_sel:    db "> ", 0
msg_unsel:  db "  ", 0
msg_count1: db "Auto boot in ", 0
msg_count2: db "s", 0
menu_items:
        dd  msg_opt0
        dd  msg_opt2
msg_opt0: db "Boot CoreZ OS", 0
msg_opt2: db "Reboot", 0

mb_cmdline:     db "root=/dev/sda2 console=tty0", 0
mb_loader_name: db "CoreZ Bootloader", 0
bits    16
set_palette:
        pusha
        mov     dx, 0x3C8
        mov     al, 0
        out     dx, al
        mov     si, palette
        mov     cx, 48
        mov     dx, 0x3C9
.l:
        mov     al, [si]
        out     dx, al
        inc     si
        dec     cx
        jnz     .l
        popa
        ret

palette:
        db 0,0,0
        db 0,0,42
        db 0,42,0
        db 0,42,42
        db 42,0,0
        db 42,0,42
        db 42,21,0
        db 42,42,42
        db 21,21,21
        db 21,21,63
        db 21,63,21
        db 21,63,63
        db 63,21,21
        db 63,21,63
        db 63,63,21
        db 63,63,63

l_readsec:
        push    si
        mov     word [dap+0x02], 1
        mov     si, ax
        mov     ax, word [l_buf]
        mov     word [dap+0x04], ax
        mov     word [dap+0x06], 0
        mov     word [dap+0x08], si
        mov     word [dap+0x0C], 0
        call    l_retry13
        pop     si
        ret

l_retry13:
        mov     byte [l_retryn], 5
.try:
        mov     si, dap
        mov     dl, byte [l_drive]
        mov     ah, 0x42
        int     0x13
        jnc     .chk
        jmp     .fail
.chk:
        test    ah, ah
        jz      .done
.fail:
        dec     byte [l_retryn]
        jz      .err
        xor     ah, ah
        int     0x13
        jmp     .try
.done:
        clc
        ret
.err:
        stc
        ret

kmsg:   db      0x0A, 0x0A, "kernel load error", 0
kmsg2:  db      0x0A, 0x0A, "kernel too large", 0

IDT0:
    %rep 256
        dw  default_handler
        dw  0x08
        db  0
        db  0x8E
        dw  0
    %endrep

IDTR0:
    dw  256*8 - 1
    dd  IDT0

default_handler:
    bits 32
    cli
.h:
    hlt
    jmp .h
    bits 16

%if (default_handler - $$) >= (0x10000 - 0xC200)
        %error "default_handler past 64KB: IDT gate offsets only encode the low 16 bits"
%endif

GDT0:
    db 0, 0, 0, 0, 0, 0, 0, 0
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF

        dw      0
GDTR0:
        dw      8*3-1
        dd      GDT0

        align   8
GDT64:
    dq 0x0000000000000000       
    dq 0x00209A0000000000      
    dq 0x00CF92000000FFFF     
    dq 0x00CF9A000000FFFF       
GDT64_LEN equ $ - GDT64

GDTR64:
    dw GDT64_LEN - 1
    dd GDT64

        alignb  16
