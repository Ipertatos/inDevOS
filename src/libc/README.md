# Freestanding libc headers for the inDevOS kernel

This directory contains the *minimal* C library headers the kernel and its
vendored code (Lua 5.4) compile against. The kernel is freestanding: it links no
host libc, so every function declared here is implemented in-kernel
(`src/mem.c`, `src/string.c`, `src/libc_shim.c`, `src/term.c` and the
`setjmp`/`longjmp` pair in `src/asmutils.asm`).

## Why this directory exists

Historically the build only worked because the host compiler's C library
headers happened to be on the include path:

* On Linux/CI, `gcc` (a *host* compiler, not a cross compiler) found
  `/usr/include/string.h`, `<math.h>`, `<setjmp.h>`, ... and with
  `-ffreestanding` those declarations were enough to compile, while the
  *implementations* came from the kernel itself.
* On Windows that trick cannot work: MinGW's `ld` cannot link ELF64 at all, and
  clang targeting `x86_64-unknown-none-elf` has no host headers to borrow.

`src/libc_shim.c` already implemented the glibc ABI pieces this depended on
(`__ctype_b_loc`, `__ctype_tolower_loc`, `__ctype_toupper_loc`,
`__errno_location`), so these headers reproduce the declaration side and the
kernel builds the same way on every host.

## Layout

| Header | Notes |
| --- | --- |
| `string.h` | delegates to the canonical `src/string.h`, adds `strerror` |
| `stdio.h` | delegates to `src/stdio.h`, adds the `FILE`/stream surface |
| `math.h` | pulls in `src/math.h` (macros) and declares the shim's maths functions |
| `stdlib.h` | `malloc`/`free`/`realloc`/`calloc`/`abort`/`strtol`/`strtod`/... |
| `ctype.h` | glibc-compatible macro interface over the shim's ctype tables |
| `errno.h` | glibc-style `errno` through `__errno_location()` |
| `setjmp.h` | 64-byte `jmp_buf` matching `src/asmutils.asm` |
| `locale.h` | `struct lconv` + `localeconv()` used by Lua's number parsing |
| `signal.h` | `sig_atomic_t` (Lua's `l_signalT`) and constants |
| `time.h` | `time_t`/`clock_t`/`struct tm`; `time()` and `clock()` are real |
| `assert.h` | `assert()` routes to the kernel's `abort()` |
| `memory.h` | non-standard alias for `string.h` (used by `src/simple_img.c`) |

Compiler-provided headers (`stddef.h`, `stdarg.h`, `stdint.h`, `stdbool.h`,
`limits.h`, `float.h`) are deliberately *not* shadowed here -- clang and GCC
both ship valid freestanding versions.

## Adding a standard-library call

1. Add the declaration to the relevant header here.
2. Add the implementation to `src/libc_shim.c` (or the file that already owns
   that area) -- a declaration alone will fail at link time, by design.
