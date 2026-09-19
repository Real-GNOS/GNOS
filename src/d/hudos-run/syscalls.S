// syscalls + crt0 (aarch64)
// sys6(n, a,b,c,d,e): x8=n; x0..x5=a..e; svc#0
.section .text
.global sys6
.type sys6, %function
sys6:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    svc #0
    ret
.size sys6, .-sys6

// sys7(n, a..f): 6 个实参 (mmap 需要 fd+offset)
.global sys7
.type sys7, %function
sys7:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    mov x6, x7
    svc #0
    ret
.size sys7, .-sys7

.global _start
.type _start, %function
_start:
    ldr x0, [sp]        // argc
    add x1, sp, #8      // argv
    bl main_c
    mov x8, #93         // exit(rc)
    svc #0
.size _start, .-_start
