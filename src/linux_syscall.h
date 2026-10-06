/*
 * linux_syscall.h - Linux x86-64 syscall ABI layer for inDevOS.
 *
 * Programs that execute the SYSCALL instruction enter syscall_entry
 * (syscallasm.asm), which builds a linux_syscall_frame_t on the per-process
 * kernel stack and calls linux_syscall_dispatch().  Numbers, argument
 * registers, structure layouts and -errno returns follow Linux x86-64, so a
 * statically linked musl binary runs unmodified.
 *
 * The private inDevOS ABI (include/syscalls.h, INT 0x80) is untouched.
 */
#ifndef LINUX_SYSCALL_H
#define LINUX_SYSCALL_H

#include "types.h"

/* Register frame pushed by syscall_entry; field order = memory order. */
typedef struct linux_syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t a6;            /* r9  */
    uint64_t a5;            /* r8  */
    uint64_t a4;            /* r10 */
    uint64_t a3;            /* rdx */
    uint64_t a2;            /* rsi */
    uint64_t a1;            /* rdi */
    uint64_t nr;            /* rax on entry */
    uint64_t user_rflags;   /* r11 */
    uint64_t user_rip;      /* rcx */
    uint64_t user_rsp;
} linux_syscall_frame_t;

/* Called from syscall_entry with interrupts enabled; returns rax. */
uint64_t linux_syscall_dispatch(linux_syscall_frame_t *f);

/* Log every Linux syscall to serial (off by default; noisy). */
extern bool linux_syscall_trace;

/* Mirror the first LINUX_STDOUT_MIRROR_MAX bytes a Linux process writes to
 * stdout/stderr onto the serial log (on by default; handy for headless
 * testing, capped so chatty programs are not slowed down by the UART). */
extern bool linux_stdout_mirror;
#define LINUX_STDOUT_MIRROR_MAX 4096

#endif /* LINUX_SYSCALL_H */
