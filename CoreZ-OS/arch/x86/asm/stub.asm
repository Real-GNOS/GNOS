[bits 64]
section .text

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro
%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common_stub
%endmacro
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31
extern isr_handler
isr_common_stub:
    push gs
    push qword 0x40
    pop  gs
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov  rdi, rsp
    mov  rbp, rsp
    and  rsp, -16
    sub  rsp, 512
    fxsave [rsp]
    call isr_handler
    fxrstor [rsp]
    mov  rsp, rbp

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    pop  gs                  
    add  rsp, 16             
    iretq
%macro IRQ 2
global irq%1
irq%1:
    push qword 0
    push qword %2
    jmp irq_common_stub
%endmacro
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

global ipi_resched
ipi_resched:
    push qword 0
    push qword 0x82
    jmp irq_common_stub

extern irq_handler
irq_common_stub:
    push gs                
    push qword 0x40
    pop  gs                   
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov  rdi, rsp
    mov  rbp, rsp
    and  rsp, -16
    sub  rsp, 512
    fxsave [rsp]
    call irq_handler
    fxrstor [rsp]
    mov  rsp, rbp
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    pop  gs                  
    add  rsp, 16
    iretq
global intr_exit
intr_exit:
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    pop  gs
    add  rsp, 16
    iretq
global default_handler
default_handler:
    push qword 0
    push qword 0xFFFF
    jmp  isr_common_stub
global syscall_0x80
syscall_0x80:
    push qword 0
    push qword 0x80
    jmp  syscall_common_stub
extern syscall_handler
syscall_common_stub:
    push gs                  
    push qword 0x40
    pop  gs                 
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov  rdi, rsp
    mov  rbp, rsp
    and  rsp, -16
    call syscall_handler
    mov  rsp, rbp
    mov  [rsp + 14*8], rax   
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    pop  gs                 
    add  rsp, 16
    iretq

global syscall_entry
extern syscall_kstack_top_data

syscall_entry:
    cli
    mov [rel syscall_user_rsp_slot], rsp
    mov rsp, [rel syscall_kstack_top_data]
    push qword 0x23
    push qword [rel syscall_user_rsp_slot]
    push r11
    push qword 0x33
    push rcx
    push qword 0
    push qword 0x81
    jmp syscall_common_stub
section .data
syscall_user_rsp_slot: dq 0
