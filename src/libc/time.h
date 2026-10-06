#ifndef KERNEL_LIBC_TIME_H
#define KERNEL_LIBC_TIME_H

/*
 * Freestanding <time.h>.
 *
 * src/libc_shim.c implements time() and clock().  The rest is declared for
 * source compatibility with vendored code; nothing in the kernel calls it.
 */

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000L

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t  time(time_t *tloc);
clock_t clock(void);

double   difftime(time_t end, time_t start);
struct tm *localtime(const time_t *timer);
struct tm *gmtime(const time_t *timer);
time_t   mktime(struct tm *timeptr);
size_t   strftime(char *s, size_t maxsize, const char *format, const struct tm *timeptr);

#endif /* KERNEL_LIBC_TIME_H */
