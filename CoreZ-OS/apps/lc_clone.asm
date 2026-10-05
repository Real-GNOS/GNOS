; clone 的用户态包装：父进程返回 tid，子进程跳入线程入口函数。
;
; 内核 raw clone 语义与 Linux 一致——子进程从 `int 0x80` 的返回点原地恢复，
; 共享父的 rip。C 包装函数对此不可用：子进程随后执行 `ret`，弹到的是新栈
; 上的未初始化数据。因此父子路径必须在汇编里分叉：子进程从新栈弹出
; （入口函数, 参数）后跳入入口函数，与 glibc/musl 的 clone 包装同构。
;
; int32_t __lc_clone_raw(uint64_t nr, int (*fn)(void *), void *stack_top,
;                        uint32_t flags, void *arg)
;   SysV 传入: rdi=nr, rsi=fn, rdx=stack_top, ecx=flags, r8=arg
[bits 64]
section .text

global __lc_clone_raw
__lc_clone_raw:
        push    rbx
        ; 在子栈顶布置 [入口函数][参数]（再留 8 字节使子进程入口处
        ; rsp % 16 == 8，满足 SysV 调用约定）
        sub     rdx, 24
        mov     [rdx], rsi              ; fn
        mov     [rdx + 8], r8           ; arg
        mov     eax, edi                ; syscall 号
        mov     ebx, ecx                ; arg1 = flags
        mov     ecx, edx                ; arg2 = 子栈指针
        int     0x80
        test    eax, eax
        jnz     .parent                 ; 父进程/出错：返回 tid 或 -1
        ; 子进程：rsp 已由内核设为新栈，[rsp]=fn, [rsp+8]=arg
        pop     rax
        pop     rdi
        jmp     rax
.parent:
        pop     rbx
        ret
