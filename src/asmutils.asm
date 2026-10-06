[BITS 64]
global stack_dump
stack_dump:
    push rdi
    mov rdi, rsp
    call dump_str
    pop rdi
    ret

extern dump_str

; ----------------------------------------------------------------------------
; setjmp/longjmp for the kernel's freestanding libc shim (used by Lua error
; handling). System V AMD64: callee-saved regs are rbx, rbp, r12-r15, plus
; rsp and the return address. Buffer layout (8 qwords, 64 bytes):
;   [0]=rbx [8]=rbp [16]=r12 [24]=r13 [32]=r14 [40]=r15 [48]=rsp' [56]=rip
; glibc's jmp_buf is larger than 64 bytes, so writing into a caller-provided
; glibc jmp_buf is safe.
; ----------------------------------------------------------------------------
global setjmp
global _setjmp
setjmp:
_setjmp:
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]        ; rsp as it will be after we return
    mov [rdi + 48], rax
    mov rax, [rsp]            ; saved return address
    mov [rdi + 56], rax
    xor eax, eax              ; direct call returns 0
    ret

global longjmp
global _longjmp
longjmp:
_longjmp:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov eax, esi              ; return value for the resumed setjmp
    test eax, eax
    jnz .have_val
    mov eax, 1                ; longjmp(env, 0) must make setjmp return 1
.have_val:
    jmp qword [rdi + 56]