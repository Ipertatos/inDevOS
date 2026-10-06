#ifndef KERNEL_LIBC_SETJMP_H
#define KERNEL_LIBC_SETJMP_H

/*
 * Freestanding <setjmp.h>.
 *
 * setjmp/longjmp are implemented in src/asmutils.asm (real register save and
 * restore, System V AMD64).  The buffer must be at least 64 bytes; the
 * assembly stores 8 qwords in this order:
 *   [0]=rbx [8]=rbp [16]=r12 [24]=r13 [32]=r14 [40]=r15 [48]=rsp [56]=rip
 *
 * Lua's error handling (luaconf.h LUAI_THROW/LUAI_TRY) relies on these.
 */

typedef unsigned long jmp_buf[8];
typedef jmp_buf sigjmp_buf;

int  setjmp(jmp_buf env);

/* longjmp does not return; saying so matches the host <setjmp.h> and lets the
 * compiler treat code after it as unreachable (Lua's luaD_throw relies on this
 * and warns "noreturn function does return" without it). */
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* POSIX aliases; both names are exported by asmutils.asm. */
int  _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* KERNEL_LIBC_SETJMP_H */
