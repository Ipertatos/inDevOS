#ifndef KERNEL_LIBC_ERRNO_H
#define KERNEL_LIBC_ERRNO_H

/*
 * Freestanding <errno.h>.
 *
 * Uses the glibc ABI that src/libc_shim.c already implements
 * (__errno_location), so vendored code compiled against this header behaves
 * the same as it did against the host <errno.h> in the Linux build.
 * Error numbers match Linux/x86-64.
 */

extern int *__errno_location(void);

#define errno (*__errno_location())

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define ENODEV      19
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOSPC      28
#define ESPIPE      29
#define EROFS       30
#define EPIPE       32
#define EDOM        33
#define ERANGE      34

#endif /* KERNEL_LIBC_ERRNO_H */
