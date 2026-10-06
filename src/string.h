#ifndef STRING_H
#define STRING_H

/*
 * Canonical kernel string/memory header.
 *
 * All standard C string/memory functions used by kernel code are
 * implemented in-kernel (mem.c, string.c, libc_shim.c).  Include this
 * header instead of the host <string.h>.
 */

#include <stddef.h>
#include <stdint.h>

/* memory (mem.c) */
void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* strings (string.c, libc_shim.c) */
size_t strlen(const char *str);
size_t strcspn(const char *str, const char *reject);
size_t strspn(const char *s, const char *accept);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcoll(const char *s1, const char *s2);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept);

/* numeric conversion (libc_shim.c, cmd.c, string.c) */
long     strtol(const char *nptr, char **endptr, int base);
double   strtod(const char *nptr, char **endptr);
int      atoi(const char *s);

/* kernel-specific helpers (string.c) */
void     kstrncpy(char *dst, const char *src, size_t n);
int      kstrcmp(const char *s1, const char *s2);
int      kstrncmp(const char *s1, const char *s2, size_t n);
uint32_t kstrtoul(const char *str, char **endptr, int base);

#endif /* STRING_H */
