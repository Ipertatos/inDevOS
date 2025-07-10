#ifndef STRING_H
#define STRING_H

#include <stddef.h>

size_t strlen(const char *str);
size_t strcspn(const char *str, const char *reject);
size_t strspn(const char *s, const char *accept);

#endif 