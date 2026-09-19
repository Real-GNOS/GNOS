// hudos-run 服务分发器 v5 (aarch64) — 帧缓冲 + BMP 导出
// 头部(loader 拷贝后填入): +0 W(u32) +4 H(u32) +8 FB_BASE(u64), 代码入口 +0x10
// 状态字(loader 映射 8192B, 代码之后): +0x400 dirty +0x404 acc_ms +0x408 dumps
// 固定加载于 0x20080000 (=CODE_AREA)。网关: x0=号, x1..x5=参数, 返回 x0
.set HDR,   0x20080000
.set H_W,   0
.set H_H,   4
.set H_FB,  8
.set ST_DIRTY, 0x400
.set ST_ACC,  0x404
.set ST_DUMPS, 0x408
.set DUMP_CAP, 12
.set DUMP_MS,  200
.section .text
.balign 4
.word 0
.word 0
.quad 0
.balign 4
.global _dispatch
.type _dispatch, %function
// 进入: 保存 callee-saved x19-x28(guest wrapper 依赖), 单出口 done
_dispatch:
    sub sp, sp, #0x50
    stp x19, x20, [sp, #0x40]
    stp x21, x22, [sp, #0x30]
    stp x23, x24, [sp, #0x20]
    stp x25, x26, [sp, #0x10]
    stp x27, x28, [sp]
    cmp x0, #56
    b.ne 1f
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    svc #0
    b 99f
1:  cmp x0, #57
    b.ne 2f
    mov x8, x0
    mov x0, x1
    svc #0
    b 99f
2:  cmp x0, #62
    b.ne 3f
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    svc #0
    b 99f
3:  cmp x0, #63
    b.ne 4f
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    svc #0
    b 99f
4:  cmp x0, #64
    b.ne 5f
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    svc #0
    b 99f
5:  cmp x0, #93            // exit: 若脏则先导出
    b.ne 6f
    mov x19, x1            // 保存 code
    bl _maybe_dump_force
    mov x8, #93
    mov x0, x19
    svc #0
    b 99f
// ---- 自定义段 ----
6:  cmp x0, #0x190         // gop_info(out=x1)
    b.ne 7f
    adrp x9, HDR
    ldr w10, [x9, #H_W]
    ldr w11, [x9, #H_H]
    ldr x12, [x9, #H_FB]
    str x12, [x1]
    str w10, [x1, #8]
    str w11, [x1, #0x10]
    lsl w13, w10, #2
    str w13, [x1, #0x18]
    mov w14, #32
    str w14, [x1, #0x20]
    bl _mark_dirty
    mov x0, #0
    b 99f
7:  cmp x0, #0x191         // gop_fill(x1=x,x2=y,x3=w,x4=h,w5=color)
    b.ne 8f
    adrp x9, HDR
    ldr w20, [x9, #H_W]
    ldr w21, [x9, #H_H]
    ldr x22, [x9, #H_FB]
    mov w23, #0
60: cmp w23, w4
    b.ge 62f
    add w24, w2, w23
    cmp w24, #0
    b.lt 61f
    cmp w24, w21
    b.ge 61f
    mov w25, #0
63: cmp w25, w3
    b.ge 61f
    add w26, w1, w25
    cmp w26, #0
    b.lt 64f
    cmp w26, w20
    b.ge 64f
    madd w27, w24, w20, w26
    lsl w27, w27, #2
    sxtw x28, w27
    str w5, [x22, x28]
64: add w25, w25, #1
    b 63b
61: add w23, w23, #1
    b 60b
62: bl _mark_dirty
    mov x0, #0
    b 99f
8:  cmp x0, #0x192         // gop_pix(x1=x, x2=y, w3=color)
    b.ne 9f
    adrp x9, HDR
    ldr w20, [x9, #H_W]
    ldr w21, [x9, #H_H]
    ldr x22, [x9, #H_FB]
    cmp w1, #0
    b.lt 65f
    cmp w2, #0
    b.lt 65f
    cmp w1, w20
    b.ge 65f
    cmp w2, w21
    b.ge 65f
    madd w23, w2, w20, w1
    lsl w23, w23, #2
    sxtw x24, w23
    str w3, [x22, x24]
65: bl _mark_dirty
    mov x0, #0
    b 99f
9:  cmp x0, #0x195         // sleep_ms(ms=x1) 后按节流导出
    b.ne 10f
    mov x19, x1            // 保存 ms
    sub sp, sp, #16
    ldr x9, =1000000
    mul x8, x19, x9
    ldr x9, =1000000000
    udiv x10, x8, x9
    msub x11, x10, x9, x8
    str x10, [sp]
    str x11, [sp, #8]
    mov x0, sp
    mov x1, xzr
    mov x8, #101
    svc #0
    add sp, sp, #16
    bl _maybe_dump_acc      // x0 = 本次 ms
    mov x0, #0
    b 99f
10: cmp x0, #0x199         // con_put(ch=x3): echo fd2
    b.ne 11f
    sub sp, sp, #16
    strb w3, [sp]
    mov x0, #2
    mov x1, sp
    mov x2, #1
    mov x8, #64
    svc #0
    add sp, sp, #16
    mov x0, #0
    b 99f
11: cmp x0, #0x1a8         // unlink(path=x1) -> unlinkat
    b.ne 12f
    mov x8, #35
    mov x0, #-100
    mov x2, #0
    svc #0
    b 99f
12: cmp x0, #0x1ab         // copy(dst=x1,src=x2,n=x3)
    b.ne 13f
    cbz x3, 14f
    sub x9, x1, #1
    sub x10, x2, #1
15: ldrb w11, [x10, #1]!
    strb w11, [x9, #1]!
    subs x3, x3, #1
    b.ne 15b
14: mov x0, #0
    b 99f
13: mov x0, #0
    b 99f
99: ldp x27, x28, [sp]
    ldp x25, x26, [sp, #0x10]
    ldp x23, x24, [sp, #0x20]
    ldp x21, x22, [sp, #0x30]
    ldp x19, x20, [sp, #0x40]
    add sp, sp, #0x50
    ret
.size _dispatch, .-_dispatch

// ---- 内部工具(假定已在 _dispatch 保存过 x19-x28) ----
// _mark_dirty: dirty=1 (x9,x10)
.type _mark_dirty, %function
_mark_dirty:
    adrp x9, HDR
    mov w10, #1
    str w10, [x9, #ST_DIRTY]
    ret
.size _mark_dirty, .-_mark_dirty

// _maybe_dump_force: 脏则直接导出(exit 用)
.type _maybe_dump_force, %function
_maybe_dump_force:
    adrp x9, HDR
    ldr w10, [x9, #ST_DIRTY]
    cbz w10, 30f
    ldr w11, [x9, #ST_DUMPS]
    cmp w11, #DUMP_CAP
    b.ge 30f
    mov x0, #1
    bl _dump_bmp
30: ret
.size _maybe_dump_force, .-_maybe_dump_force

// _maybe_dump_acc: 输入 x0=ms; 累计>=DUMP_MS 且脏且未超限则导出
.type _maybe_dump_acc, %function
_maybe_dump_acc:
    adrp x9, HDR
    ldr w10, [x9, #ST_ACC]
    add w10, w10, w0
    ldr w11, [x9, #ST_DIRTY]
    ldr w12, [x9, #ST_DUMPS]
    cmp w10, #DUMP_MS
    b.lt 40f
    cbz w11, 41f
    cmp w12, #DUMP_CAP
    b.ge 41f
    str wzr, [x9, #ST_ACC]
    mov x0, #1
    bl _dump_bmp
    ret
40: str w10, [x9, #ST_ACC]
41: ret
.size _maybe_dump_acc, .-_maybe_dump_acc

// _dump_bmp: 把帧缓冲写为 /tmp/hudos-fb.bmp (32bpp XRGB, top-down)
.type _dump_bmp, %function
_dump_bmp:
    sub sp, sp, #0x60
    stp x29, x30, [sp, #0x50]
    add x29, sp, #0x50
    adrp x19, HDR
    ldr w20, [x19, #H_W]        // W
    ldr w21, [x19, #H_H]        // H
    ldr x22, [x19, #H_FB]       // fb
    // openat("/tmp/hudos-fb.bmp", O_WRONLY|O_CREAT|O_TRUNC, 0644)
    adrp x1, L_bmp
    add x1, x1, :lo12:L_bmp
    mov x0, #-100
    mov x2, #0x241
    mov x3, #0x1A4
    mov x8, #56
    svc #0
    cmp x0, #0
    b.lt 90f
    mov x23, x0                 // fd
    // 组装 54B BMP 头(全定长小端, 无十进制转换)
    madd x24, x21, x20, xzr     // 面积
    lsl x24, x24, #2            // 像素字节 = W*H*4
    mov x25, #54
    add x26, x24, x25           // 文件总大小
    mov w27, 0x4D42             // 'BM'
    strh w27, [sp]
    str w26, [sp, #2]           // bfSize
    str wzr, [sp, #6]           // 保留
    str wzr, [sp, #10]
    str w25, [sp, #14]          // bfOffBits=54
    mov w27, #40
    str w27, [sp, #18]          // biSize
    str w20, [sp, #22]          // biWidth
    neg w28, w21                // 负高 = top-down
    str w28, [sp, #26]
    mov w27, #1
    strh w27, [sp, #30]         // planes
    mov w27, #32
    strh w27, [sp, #32]         // bpp
    str wzr, [sp, #34]
    str wzr, [sp, #38]
    str wzr, [sp, #42]
    str wzr, [sp, #46]
    str wzr, [sp, #50]
    // write(fd, hdr, 54)
    mov x0, x23
    mov x1, sp
    mov x2, #54
    mov x8, #64
    svc #0
    // write(fd, fb, W*H*4)
    mov x0, x23
    mov x1, x22
    mov x2, x24
    mov x8, #64
    svc #0
    // close + dumps++
    mov x0, x23
    mov x8, #57
    svc #0
    adrp x9, HDR
    ldr w10, [x9, #ST_DUMPS]
    add w10, w10, #1
    str w10, [x9, #ST_DUMPS]
    mov w10, #0
    str w10, [x9, #ST_DIRTY]
90: ldp x29, x30, [sp, #0x50]
    add sp, sp, #0x60
    ret
.size _dump_bmp, .-_dump_bmp

.section .rodata
L_bmp: .ascii "/tmp/hudos-fb.bmp\0"
