[BITS 64]

; Thread context switching for kernel-level cooperative multitasking
; All threads run in ring 0 with the same segment selectors, so we only
; need to save/restore general purpose registers and the stack pointer.

; void thread_switch_asm(thread_context_t *from, thread_context_t *to)
; rdi = from context pointer (can be NULL for first switch)
; rsi = to context pointer
global thread_switch_asm
thread_switch_asm:
    ; Check if 'from' is NULL (first switch, nothing to save)
    test rdi, rdi
    jz .restore_only
    
    ; Save current context to 'from' structure
    ; Context structure layout:
    ;   0x00: r15, 0x08: r14, 0x10: r13, 0x18: r12
    ;   0x20: r11, 0x28: r10, 0x30: r9,  0x38: r8
    ;   0x40: rdi, 0x48: rsi, 0x50: rbp, 0x58: rsp
    ;   0x60: rbx, 0x68: rdx, 0x70: rcx, 0x78: rax
    ;   0x80: rip, 0x88: rflags
    
    mov [rdi + 0x00], r15
    mov [rdi + 0x08], r14
    mov [rdi + 0x10], r13
    mov [rdi + 0x18], r12
    mov [rdi + 0x20], r11
    mov [rdi + 0x28], r10
    mov [rdi + 0x30], r9
    mov [rdi + 0x38], r8
    ; rdi saved later (we're using it)
    mov [rdi + 0x48], rsi
    mov [rdi + 0x50], rbp
    mov [rdi + 0x60], rbx
    mov [rdi + 0x68], rdx
    mov [rdi + 0x70], rcx
    mov [rdi + 0x78], rax
    
    ; Save the stack pointer
    mov [rdi + 0x58], rsp
    
    ; Save return address as rip
    mov rax, [rsp]          ; Get return address from stack
    mov [rdi + 0x80], rax   ; Save as rip
    
    ; Save flags
    pushfq
    pop rax
    mov [rdi + 0x88], rax
    
    ; Now save rdi (we can use rax now)
    ; Note: rdi holds the 'from' context pointer (first parameter),
    ; which IS the correct value of rdi at the time of the call.
    mov [rdi + 0x40], rdi
    
.restore_only:
    ; Restore context from 'to' structure (rsi)
    ; rax holds the to_context pointer throughout the restore sequence.
    ; We must defer restoring rsi and rax until rsp and rip are set up.
    mov rax, rsi            ; rax = to context
    
    ; Restore flags first
    mov rcx, [rax + 0x88]
    push rcx
    popfq
    
    ; Restore general purpose registers (defer rsi, rsp, rax)
    mov r15, [rax + 0x00]
    mov r14, [rax + 0x08]
    mov r13, [rax + 0x10]
    mov r12, [rax + 0x18]
    mov r11, [rax + 0x20]
    mov r10, [rax + 0x28]
    mov r9,  [rax + 0x30]
    mov r8,  [rax + 0x38]
    mov rdi, [rax + 0x40]
    ; rsi deferred -- rax still points to to_context
    mov rbp, [rax + 0x50]
    mov rbx, [rax + 0x60]
    mov rdx, [rax + 0x68]
    mov rcx, [rax + 0x70]
    
    ; Switch to the target stack (rax still holds to_context pointer)
    mov rsp, [rax + 0x58]
    
    ; Push target rip onto the new stack for ret
    push qword [rax + 0x80]
    
    ; Now restore rsi and rax last
    mov rsi, [rax + 0x48]
    mov rax, [rax + 0x78]
    
    ret                     ; Return to new thread's rip

; Thread startup function
; This is called when a new thread starts for the first time
; The stack should be set up with: entry_point, argument
extern thread_exit

global thread_start_asm
thread_start_asm:
    ; Enable interrupts (threads should run with interrupts enabled)
    sti
    
    ; Pop the entry function and argument
    pop rax                 ; Entry function address
    pop rdi                 ; Thread argument
    
    ; Call the thread entry function
    call rax
    
    ; Thread function returned - call C cleanup function to properly
    ; remove the thread from the scheduler and free resources.
    call thread_exit
    
    ; thread_exit should never return, but guard against it
    cli
.halt_loop:
    hlt
    jmp .halt_loop
