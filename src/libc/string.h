#ifndef KERNEL_LIBC_STRING_H
#define KERNEL_LIBC_STRING_H

/*
 * Freestanding <string.h>.
 *
 * The canonical kernel declarations live in src/string.h; this header adds the
 * extra entry point that vendored code (Lua) expects from <string.h>.
 * Every function declared here is implemented in the kernel (mem.c, string.c,
 * libc_shim.c) -- nothing is borrowed from a host libc.
 */

#include "../string.h"

/* stdlib-shaped string helper (libc_shim.c) */
char *strerror(int errnum);

#endif /* KERNEL_LIBC_STRING_H */
