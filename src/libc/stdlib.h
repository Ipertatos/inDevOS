#ifndef KERNEL_LIBC_STDLIB_H
#define KERNEL_LIBC_STDLIB_H

/*
 * Freestanding <stdlib.h>.
 *
 * Implemented by src/libc_shim.c on top of the kernel allocator (kalloc/kfree):
 * malloc/calloc/realloc/free, abort, abs, atoi, strtol, strtod.
 * The remaining declarations are provided for source compatibility only --
 * nothing in the kernel calls them.
 */

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

void   abort(void) __attribute__((noreturn));
void   exit(int status) __attribute__((noreturn));
int    atexit(void (*func)(void));

int    abs(int x);
int    atoi(const char *s);
long   strtol(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);

char  *getenv(const char *name);
int    system(const char *command);

#endif /* KERNEL_LIBC_STDLIB_H */
