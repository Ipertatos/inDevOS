default rel

[GLOBAL s_setgdt]
[GLOBAL s_flushgdt]
[GLOBAL s_settss]

extern gdt_pointer

s_setgdt:
    mov [gdt_pointer], di
    mov [gdt_pointer+2], rsi
    lgdt [gdt_pointer]
    mov rax, 0x10 ;kdata offset
    mov ds, rax
    mov es, rax
    mov gs, rax
    mov fs, rax
    mov ss, rax
    pop rax
    push 0x08   ;kcode offset
    push rax
    retfq

s_settss:
    mov ax, 0x28 ; tss offset in gdt
    ltr ax
    ret