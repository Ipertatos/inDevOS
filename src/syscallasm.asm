[BITS 64]

extern syscall_handler
extern syscall_stash_int80_fork_frame

; ===========================================================================
; enter_usermode(uint64_t entry_rip, uint64_t user_rsp)
;
; Builds an iretq frame on the kernel stack and executes iretq to jump to
; Ring 3 at the given entry point.  Does NOT return.
;
; GDT layout (must match gdt.c):
;   0x1B = user data  (0x18 | RPL=3)
;   0x23 = user code  (0x20 | RPL=3)
; ===========================================================================

global enter_usermode
enter_usermode:
    ; rdi = entry_rip, rsi = user_rsp
    push 0x1B           ; SS  (user data, RPL=3)
    push rsi            ; RSP (user stack)
    pushfq              ; RFLAGS
    or qword [rsp], 0x200   ; ensure IF (interrupt-enable) is set
    push 0x23           ; CS  (user code, RPL=3)
    push rdi            ; RIP (user entry point)
    iretq

; ===========================================================================
; Legacy syscall path via INT 0x80 (works from any privilege level)
;
; The CPU automatically switches to the kernel stack (TSS RSP0) on a
; privilege change, so this path is already safe for Ring 3 callers.
; ===========================================================================
global syscall_asm_handler
syscall_asm_handler:
    ; Stash user RIP/RSP/RFLAGS for SYS_FORK (iframe still at RSP).
    ; The C call clobbers caller-saved regs: rax = syscall nr, rdi/rsi/rdx/
    ; r10/r8/r9 = args. Save all of them (9 pushes keep RSP 16-aligned).
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    lea rdi, [rsp + 72]     ; skip our 9 pushes — hardware iretq frame
    call syscall_stash_int80_fork_frame
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ; Save all registers
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
    
    ; Arguments are in registers according to Linux syscall ABI:
    ; rax = syscall number
    ; rdi = arg1
    ; rsi = arg2
    ; rdx = arg3
    ; r10 = arg4 (rcx is clobbered by syscall instruction)
    ; r8  = arg5
    ; r9  = arg6
    
    ; Set up arguments for C function call (System V AMD64 ABI):
    ; RDI = syscall_num, RSI = arg1, RDX = arg2, RCX = arg3, R8 = arg4, R9 = arg5
    mov r9, r8      ; arg5 (r8 -> r9)
    mov r8, r10     ; arg4 (r10 -> r8)  
    mov rcx, rdx    ; arg3 (rdx -> rcx)
    mov rdx, rsi    ; arg2 (rsi -> rdx)
    mov rsi, rdi    ; arg1 (rdi -> rsi)
    mov rdi, rax    ; syscall_num (rax -> rdi)
    
    ; Call the C handler
    call syscall_handler
    
    ; Restore all registers except rax (which contains return value)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ; Don't pop rax - it contains the return value
    add rsp, 8      ; Skip the saved rax
    
    iretq

; ===========================================================================
; SYSCALL instruction entry  ==  Linux x86-64 ABI  (linux_syscall.c)
;
; The private inDevOS ABI stays on INT 0x80 (syscall_asm_handler above); every
; program that executes the SYSCALL instruction is treated as a Linux binary.
;
; On SYSCALL entry (hardware behaviour):
;   RCX = user RIP, R11 = user RFLAGS, RAX = Linux syscall number
;   RDI, RSI, RDX, R10, R8, R9 = arguments 1..6
;   CS/SS loaded from STAR (kernel), RSP unchanged (still the USER stack!)
;   IF/DF/TF/AC cleared by SFMASK
;
; GS discipline: swapgs is used ONLY for the three instructions that fetch the
; per-CPU kernel stack, with interrupts still disabled, and is undone before
; any C code runs.  So outside this stub the invariant is always
;   GS_BASE = user value (0), KERNEL_GS_BASE = &cpu_gs[cpu]
; exactly as on the INT 0x80 path, and nothing that leaves the kernel through
; longjmp (exit, exec, Ctrl+C, faults) has to fix GS up afterwards.
;
; Per-CPU data (cpu_gs_t in syscall.c, reached through KERNEL_GS_BASE):
;   gs:0  = kernel_rsp   (kernel stack top, kept in sync with TSS RSP0)
;   gs:8  = user_rsp     (scratch)
;
; Frame passed to linux_syscall_dispatch (linux_syscall_frame_t, low->high):
;   r15 r14 r13 r12 rbp rbx | r9 r8 r10 rdx rsi rdi | rax(nr) | r11 rcx rsp
; ===========================================================================
extern linux_syscall_dispatch

global syscall_entry
syscall_entry:
    swapgs
    mov [gs:8], rsp             ; stash user RSP
    mov rsp, [gs:0]             ; per-CPU kernel stack
    push qword [gs:8]           ; frame.user_rsp
    swapgs                      ; GS back to the user value right away

    push rcx                    ; frame.user_rip
    push r11                    ; frame.user_rflags
    push rax                    ; frame.nr
    push rdi                    ; frame.a1
    push rsi                    ; frame.a2
    push rdx                    ; frame.a3
    push r10                    ; frame.a4
    push r8                     ; frame.a5
    push r9                     ; frame.a6
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15                    ; 16 qwords: RSP stays 16-byte aligned

    mov rdi, rsp                ; linux_syscall_frame_t *
    cld
    sti
    call linux_syscall_dispatch ; returns the result (or -errno) in RAX
    cli

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    add rsp, 8                  ; frame.nr (RAX holds the return value)
    pop r11                     ; user RFLAGS
    pop rcx                     ; user RIP

    ; SYSRET with a non-canonical RCX faults in ring 0 on the user stack.
    ; RCX came from the SYSCALL itself, so it is canonical unless the frame
    ; was rewritten (signal deliver sets handler RIP); refuse to sysret in
    ; that case and use iretq instead. Note: GPRs were already popped from
    ; the frame above — SYSRET only loads rip/rflags from rcx/r11; the rest
    ; (including rdi=signum for the handler) are already in the real regs.
    push rax
    mov rax, rcx
    shl rax, 16
    sar rax, 16
    cmp rax, rcx
    pop rax
    jne .iret_path

    pop rsp                     ; user RSP
    o64 sysret

.iret_path:
    ; Build an iretq frame: SS, RSP, RFLAGS, CS, RIP (user selectors, gdt.c)
    ; (S = address of the saved user RSP slot; interrupts are off)
    mov [rsp - 48], rcx         ; RIP parked at S-48, below the frame we build
    pop rcx                     ; rcx = user RSP, rsp = S+8
    push qword 0x1B             ; [S]    SS
    push rcx                    ; [S-8]  RSP
    push r11                    ; [S-16] RFLAGS
    push qword 0x23             ; [S-24] CS
    push qword [rsp - 24]       ; [S-32] RIP (reads S-48)
    iretq

; ===========================================================================
; exec_setjmp / exec_longjmp
;
; Minimal setjmp/longjmp used by exec_run_elf to save the kernel context
; before entering user mode, so that sys_exit can longjmp back to it.
;
; exec_jmpbuf layout (8 uint64_t fields at 0..56):
;   [0]  RSP (caller's stack pointer after return)
;   [8]  RBP
;   [16] RBX
;   [24] R12
;   [32] R13
;   [40] R14
;   [48] R15
;   [56] RIP (return address)
;
; exec_setjmp(exec_jmpbuf_t *buf)  -> 0 on first call, non-zero on longjmp
; exec_longjmp(exec_jmpbuf_t *buf, int val)  -> never returns
; ===========================================================================
; lx_iret_full(lx_iret_regs_t *rdi) -- return to user from rt_sigreturn of an
; IRQ-delivered handler: restore ALL GPRs (incl. rcx/r11) and iretq.
; GS is already in the invariant state (GS_BASE=user) outside the entry stub.
global lx_iret_full
lx_iret_full:
    cli
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

global exec_setjmp
exec_setjmp:
    ; rdi = pointer to exec_jmpbuf_t
    lea  rax, [rsp+8]        ; caller's RSP after return (skip the return addr)
    mov  [rdi+0],  rax
    mov  [rdi+8],  rbp
    mov  [rdi+16], rbx
    mov  [rdi+24], r12
    mov  [rdi+32], r13
    mov  [rdi+40], r14
    mov  [rdi+48], r15
    mov  rax, [rsp]          ; return address
    mov  [rdi+56], rax
    xor  eax, eax            ; return 0
    ret

global exec_longjmp
exec_longjmp:
    ; rdi = pointer to exec_jmpbuf_t, rsi = return value (must be != 0)
    mov  rbx, [rdi+16]
    mov  r12, [rdi+24]
    mov  r13, [rdi+32]
    mov  r14, [rdi+40]
    mov  r15, [rdi+48]
    mov  rbp, [rdi+8]
    mov  rsp, [rdi+0]
    mov  rax, rsi            ; value to return from exec_setjmp
    test eax, eax
    jnz  .nonzero
    inc  eax                 ; ensure non-zero (longjmp(buf,0) returns 1)
.nonzero:
    jmp  [rdi+56]            ; jump to saved return address
