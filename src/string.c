#include <stddef.h>
#include "types.h"

size_t strlen(const char *str) {
    if (!str) return 0;
    size_t len = 0;
    while (*str++) len++;
    return len;
}

size_t strcspn(const char *str, const char *reject) {
    if (!str || !reject) return 0;
    size_t count = 0;
    while (str[count]) {
        const char *r = reject;
        while (*r) {
            if (str[count] == *r) return count;
            r++;
        }
        count++;
    }
    return count;
}

size_t strspn(const char *s, const char *accept) {
    if (!s || !accept) return 0;
    size_t i = 0;
    while (s[i]) {
        const char *a = accept;
        bool match = false;
        while (*a) {
            if (s[i] == *a) { match = true; break; }
            a++;
        }
        if (!match) break;
        i++;
    }
    return i;
} 

void kstrncpy(char *dst, const char *src, size_t n) {
    if (n == 0) return;
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(*s1 - *s2);
}

// Standard C library compatibility functions for desktop environment
char* strncpy(char *dst, const char *src, size_t n) {
    kstrncpy(dst, src, n);
    return dst;
}

int kstrncmp(const char *s1, const char *s2, size_t n) {
    size_t i;
    for (i = 0; i < n && s1[i] && s2[i] && s1[i] == s2[i]; i++);
    if (i == n) return 0;
    return (int)(s1[i] - s2[i]);
}

uint32_t kstrtoul(const char *str, char **endptr, int base) {
    uint32_t result = 0;
    uint32_t digit;
    
    // Skip leading whitespace
    while (*str == ' ' || *str == '\t') str++;
    
    // Handle base
    if (base == 0) {
        if (*str == '0') {
            str++;
            if (*str == 'x' || *str == 'X') {
                base = 16;
                str++;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    }
    
    // Convert digits
    while (*str) {
        if (*str >= '0' && *str <= '9') {
            digit = *str - '0';
        } else if (*str >= 'a' && *str <= 'f' && base == 16) {
            digit = *str - 'a' + 10;
        } else if (*str >= 'A' && *str <= 'F' && base == 16) {
            digit = *str - 'A' + 10;
        } else {
            break;
        }
        
        if (digit >= base) break;
        
        result = result * base + digit;
        str++;
    }
    
    if (endptr) *endptr = (char*)str;
    return result;
} 