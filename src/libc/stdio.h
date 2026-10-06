#ifndef KERNEL_LIBC_STDIO_H
#define KERNEL_LIBC_STDIO_H

/*
 * Freestanding <stdio.h>.
 *
 * src/stdio.h (the canonical kernel header) declares printf/snprintf/vsnprintf;
 * this header adds the FILE/stream surface that vendored code (Lua's lauxlib)
 * expects.  The stream entry points are the stubs in src/libc_shim.c.
 */

#include "../stdio.h"

#include <stddef.h>
#include <stdarg.h>

/* libc_shim.c defines "struct FILE { int dummy; }"; only the tag matters. */
typedef struct FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#ifndef EOF
#define EOF (-1)
#endif
#define BUFSIZ 1024

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int    fprintf(FILE *stream, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    putchar(int c);
int    fflush(FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);

FILE  *fopen(const char *path, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *stream);
FILE  *fopen64(const char *path, const char *mode);
FILE  *freopen64(const char *path, const char *mode, FILE *stream);
int    fclose(FILE *stream);
int    getc(FILE *stream);
int    feof(FILE *stream);
int    ferror(FILE *stream);

#endif /* KERNEL_LIBC_STDIO_H */
