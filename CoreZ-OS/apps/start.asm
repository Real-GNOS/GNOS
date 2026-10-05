; 64 位用户程序 crt0 / 启动入口
; 遵循 Linux/SysV x86_64 ABI:
;   _start 入口时 rsp 指向初栈:
;     [rsp]        = argc
;     [rsp+8]      = argv[0..argc-1]
;     [rsp+8+8*argc+8] = envp[0..]
; 我们从栈读出 argc/argv/envp, 调用 main(argc, argv, envp),
[bits 64]
extern main
extern exit
extern __ssp_init

section .text
global _start
_start:
    call    __ssp_init              ; 初始化 stack canary guard
    mov     rdi, [rsp]              ; rdi = argc
    lea     rsi, [rsp + 8]          ; rsi = argv
    lea     rdx, [rsi + rdi*8 + 8]  ; rdx = envp (跳过 argv 数组及其后 NULL)
    xor     rax, rax                ; 清返回值寄存器(对齐 ABI 约定)
    call    main
    mov     rdi, rax                ; exit(status)
    call    exit
.hang:
    hlt
    jmp     .hang