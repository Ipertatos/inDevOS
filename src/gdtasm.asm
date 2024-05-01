[bits 64]

global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x30
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov ss,ax
    pop rdi
    mov rax, 0x28
    push rax    
    push rdi
    retfq

global tss_flush
tss_flush:
    mov ax, 0x48
    ltr ax
    ret