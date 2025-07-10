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