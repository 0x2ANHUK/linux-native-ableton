// emu_thunk: called from a translation stub that did
//     lea -128(%rsp),%rsp ; push %rax ; lea desc(%rip),%rax ; call *slot(%rip)
// Saves the whole integer state and ymm0-15, calls emu_interp(frame, desc),
// restores (the interpreter may have changed any of it), returns to the stub.
    .text
    .globl emu_thunk
    .hidden emu_thunk
    .type emu_thunk,@function
    .p2align 4
emu_thunk:
    pushfq
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rdi
    push %rsi
    push %rbp
    push %rbx
    push %rdx
    push %rcx
    mov %rsp, %rbx
    mov %rax, %rsi
    mov %rsp, %rdi
    movq _ZN3emu3tstE@gottpoff(%rip), %rax
    vmovdqu %ymm0, %fs:0(%rax)
    vmovdqu %ymm1, %fs:32(%rax)
    vmovdqu %ymm2, %fs:64(%rax)
    vmovdqu %ymm3, %fs:96(%rax)
    vmovdqu %ymm4, %fs:128(%rax)
    vmovdqu %ymm5, %fs:160(%rax)
    vmovdqu %ymm6, %fs:192(%rax)
    vmovdqu %ymm7, %fs:224(%rax)
    vmovdqu %ymm8, %fs:256(%rax)
    vmovdqu %ymm9, %fs:288(%rax)
    vmovdqu %ymm10, %fs:320(%rax)
    vmovdqu %ymm11, %fs:352(%rax)
    vmovdqu %ymm12, %fs:384(%rax)
    vmovdqu %ymm13, %fs:416(%rax)
    vmovdqu %ymm14, %fs:448(%rax)
    vmovdqu %ymm15, %fs:480(%rax)
    and $-32, %rsp
    cld
    vzeroupper
    call emu_interp
    movq _ZN3emu3tstE@gottpoff(%rip), %rax
    vmovdqu %fs:0(%rax), %ymm0
    vmovdqu %fs:32(%rax), %ymm1
    vmovdqu %fs:64(%rax), %ymm2
    vmovdqu %fs:96(%rax), %ymm3
    vmovdqu %fs:128(%rax), %ymm4
    vmovdqu %fs:160(%rax), %ymm5
    vmovdqu %fs:192(%rax), %ymm6
    vmovdqu %fs:224(%rax), %ymm7
    vmovdqu %fs:256(%rax), %ymm8
    vmovdqu %fs:288(%rax), %ymm9
    vmovdqu %fs:320(%rax), %ymm10
    vmovdqu %fs:352(%rax), %ymm11
    vmovdqu %fs:384(%rax), %ymm12
    vmovdqu %fs:416(%rax), %ymm13
    vmovdqu %fs:448(%rax), %ymm14
    vmovdqu %fs:480(%rax), %ymm15
    mov %rbx, %rsp
    pop %rcx
    pop %rdx
    pop %rbx
    pop %rbp
    pop %rsi
    pop %rdi
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    popfq
    ret
    .size emu_thunk, .-emu_thunk
    .section .note.GNU-stack,"",@progbits
