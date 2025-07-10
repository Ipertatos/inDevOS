bits 64
org 0

extern printf   ; doesn’t really exist, we hard-code the addr below
%define PRINTF  0xffffffff800163e0   ; look up in kernel.sym

segment executable
_start:
    mov     rdi, msg
    xor     eax, eax         ; SysV ABI: no vector registers
    call    PRINTF
    ret

msg db "Hello from user program!",10,0
