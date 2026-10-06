#ifndef SYS_INTERNAL_H
#define SYS_INTERNAL_H

/*
 * Shared internals for syscall subsystem files.
 *
 * syscall.c currently contains ALL syscall implementations in one 3000-line
 * file.  This header declares the shared process/fd management primitives
 * so implementations can be split into:
 *
 *   syscall.c       - dispatcher, init, MSR setup, process table
 *   (future) sys_fs.c      - filesystem syscalls
 *   (future) sys_net.c     - socket/network syscalls
 *   (future) sys_proc.c    - process/signal/memory syscalls
 *   (future) sys_sdl.c     - SDL graphics syscalls
 *
 * Subsystem files include this header to access shared state.
 */

#include "syscall.h"
#include "mem.h"
#include "uaccess.h"
#include "thread.h"
#include "ktime.h"
#include "kernel_config.h"   /* canonical MAX_PROCESSES */
#include "../include/indevos_errno.h"

/* Process table (owned by syscall.c); sized by MAX_PROCESSES from kernel_config.h */
extern process_t *get_current_process(void);
extern void set_current_process(process_t *proc);
extern process_t *exec_user_as_process(void);

/* FD management (owned by syscall.c) */
extern int allocate_fd(process_t *process);
extern void free_fd(process_t *process, int fd);

#endif /* SYS_INTERNAL_H */
