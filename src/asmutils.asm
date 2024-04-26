[BITS 64]
global stack_dump
stack_dump:
    push rdi
    mov rdi, rsp
    call dump_str
    pop rdi
    ret

extern dump_str