[BITS 64]
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    cli
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    cli
    push %1
    jmp isr_common_stub
%endmacro


;; one stub per vector (0-255); CPU pushes an error code for 8,10-14,17,21,29,30
%assign v 0
%rep 256
%if v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30
ISR_ERRCODE v
%else
ISR_NOERRCODE v
%endif
%assign v v+1
%endrep

extern isr_handler
extern syscall_asm_handler
isr_common_stub:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    cld                     ; C code (rep movs/stos in memcpy) requires DF=0
    mov rdi, rsp
    call isr_handler

    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 16
    iretq

section .data
align 8
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 256
    dq isr%[v]
%assign v v+1
%endrep
