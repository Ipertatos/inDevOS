/*
 * exec.h - Simple executable loader for inDevOS
 * Supports minimal custom header format and stub for ELF detection.
 *
 * Custom format:
 *   Offset 0: 4-byte magic "EXE!"
 *   Offset 4: 4-byte little-endian entry point offset from start of file
 *   Code/data follow immediately after optional header.
 */
#ifndef EXEC_H
#define EXEC_H

#include "types.h"
#include "syscall.h"

/* Try to load a program file from the currently mounted filesystem and execute
 * it in kernel context.
 * Returns 0 on success or negative error code on failure. */
int exec_file(const char *filename, char *const argv[]);

/* Loader phase timing: prints "[exec +Nus] phase" to the serial log relative to
 * the start of the current exec.  Callable from other loader-path code (VFS). */
void exec_dbg_mark(const char *phase);

/* Load each program's ELF symtab so user-mode faults can be symbolised?
 * Off by default: parsing it costs time on every launch and only helps when
 * debugging a crashing program (`execsym on` in the shell). */
extern bool exec_user_symbols_enabled;

/* Called by sys_exit() to return execution to the kernel shell.
 * Switches back to the kernel address space and longjmps out of
 * exec_run_elf, so the shell can continue normally. Does NOT return. */
__attribute__((noreturn))
void exec_notify_exit(int status);

/* Like exec_notify_exit but callable from non-syscall contexts (timer ISR,
 * keyboard handler, etc.) where swapgs has NOT been performed. */
__attribute__((noreturn))
void exec_force_terminate(int status);

/* True when the current thread is running a program started by exec_file
 * (Ring 3 on the caller's kernel thread): end it with exec_force_terminate
 * rather than killing the thread, which would take the shell with it. */
bool exec_current_is_sync_program(void);

/* Minimal setjmp/longjmp (assembly, syscallasm.asm) used internally. */
typedef struct {
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
} exec_jmpbuf_t;

int  exec_setjmp (exec_jmpbuf_t *buf);
__attribute__((noreturn))
void exec_longjmp(exec_jmpbuf_t *buf, int val);

/*
 * While a Ring-3 image from exec_run_elf is active, timer preemption may
 * change get_current_process() to another thread. Syscalls and #PF handling
 * must still use the exec'd process (PML4, brk, COW metadata).
 */
process_t *exec_user_as_process(void);
void exec_push_return_ctx(void);
void exec_pop_return_ctx(void);

/* Point Ring-3 PF/syscall identity at @p (used across vfork handoff). */
void exec_set_ring3_active(process_t *p);

/* True after exec_notify_exit's swapgs; sys_exec must undo it when returning
 * through a syscall path (e.g. forked child calling exec from Ring 3). */
extern bool exec_did_swapgs;

/* Spawn an ELF in a new preemptively-scheduled Ring-3 thread.
 * Returns the child PID (>0) on success, negative on error.
 * The caller is NOT blocked; the child runs when the scheduler picks it. */
int exec_spawn(const char *filename, char *const argv[]);

/* Replace the calling process image in place (same pid).  On success sets
 * proc->exec_replaced and exec_entry_{rip,rsp}; caller SYSRETs into the new
 * image.  Returns 0 or -errno.  Does not nest and does not allocate a new
 * process slot. Kernel stack of @proc is preserved. */
int exec_replace(process_t *proc, const char *filename, char *const argv[]);

/* Called by sys_exit() for spawned (thread-scheduled) processes.
 * Cleans up and lets the scheduler pick the next thread.  Does NOT return. */
__attribute__((noreturn))
void exec_spawned_exit(int status);
void exec_spawned_exit_int80(int status);

/* Resolve RIP/return address: kernel symtab or loaded user ELF symbols */
const char *exec_lookup_symbol(process_t *proc, uint64_t addr);

#endif // EXEC_H 