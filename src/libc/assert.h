#ifndef KERNEL_LIBC_ASSERT_H
#define KERNEL_LIBC_ASSERT_H

/*
 * Freestanding <assert.h>.
 *
 * There is no host libc, so a failed assertion ends in abort() (src/libc_shim.c).
 * Lua only reaches this through LUAI_ASSERT / LUA_USE_APICHECK, which the kernel
 * build does not define -- but providing it keeps vendored sources compilable.
 */

void abort(void) __attribute__((noreturn));

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : abort())
#endif

#endif /* KERNEL_LIBC_ASSERT_H */
