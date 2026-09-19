section .data
    prompt  db "CPU Vendor: ", 0
    prompt_len equ $ - prompt

    newline db 0x0A

section .bss
    vendor  resb 12          ; 厂商 ID 共 12 字节

section .text
    global _start

_start:
    ; 1. 输出提示字符串
    mov rax, 1               ; sys_write
    mov rdi, 1               ; stdout
    mov rsi, prompt
    mov rdx, prompt_len
    syscall

    ; 2. 执行一次 CPUID，leaf 0
    xor eax, eax             ; EAX = 0
    cpuid                    ; 结果: EAX=最大leaf, EBX/EDX/ECX=厂商ID

    ; 3. 把厂商ID按顺序存入缓冲区
    ;    顺序是 EBX, EDX, ECX
    mov dword [vendor], ebx
    mov dword [vendor+4], edx
    mov dword [vendor+8], ecx

    ; 4. 输出厂商ID（12字节）
    mov rax, 1               ; sys_write
    mov rdi, 1               ; stdout
    mov rsi, vendor
    mov rdx, 12
    syscall

    ; 5. 输出换行
    mov rax, 1
    mov rdi, 1
    mov rsi, newline
    mov rdx, 1
    syscall

    ; 6. 退出
    mov rax, 60              ; sys_exit
    xor edi, edi             ; 返回码 0
    syscall
