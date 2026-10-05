; 参考: Wine ntdll 的进程入口 (ldt 初始化) + Linux x86_64 SysV ABI 的 _start (crt1.o) 栈解析
[bits 64]
default rel
extern main
extern __lc_tls_init
extern __lc_terminate

section .text

global _lc_start
_lc_start:
    and     rsp, -16            ; 16 字节对齐, 满足函数调用前 rsp%16==0
    xor     ebp, ebp            ; 清栈帧指针
    ; 内核 exec 按 SysV ABI 传参: rdi=argc, rsi=argv
    lea     rdx, [rsi + rdi*8 + 8]  ; rdx = envp = argv + (argc+1)*8

    ; 由 envp 向后扫描空 qword, 定位 auxv (标准布局: auxv 紧邻 envp NULL 之后)
    mov     rcx, rdx
.lc_auxv_scan:
    cmp     qword [rcx], 0
    je      .lc_auxv_found
    add     rcx, 8
    jmp     .lc_auxv_scan
.lc_auxv_found:
    add     rcx, 8                  ; rcx = auxv 起始
    mov     [__lc_auxv], rcx

    call    __lc_tls_init

    ; SysV: rdi=argc, rsi=argv, rdx=envp
    call    main
    mov     rdi, rax                ; status = main 返回值
    call    __lc_terminate
.lc_halt:
    jmp     .lc_halt

; __lc_syscall6: 七参 (nr,a,b,c,d,e,f) SysV ABI, 重排映射到内核 six-register ABI
; 入参: rdi=nr, rsi=a, rdx=b, rcx=c, r8=d, r9=e, [rsp+8]=f
global __lc_syscall6
__lc_syscall6:
    push    rbx                
    push    rbp               
    mov     rax, rdi            ; rax = nr
    mov     rbx, rsi            ; rbx = a
    xchg    rcx, rdx            ; rcx = b, rdx = c (xchg 避免 rcx 被先覆盖)
    mov     rsi, r8             ; rsi = d
    mov     rdi, r9             ; rdi = e
    mov     rbp, [rsp + 24]     ; rbp = f
    int     0x80
    pop     rbp
    pop     rbx
    ret

section .data
global __lc_auxv
__lc_auxv: dq 0