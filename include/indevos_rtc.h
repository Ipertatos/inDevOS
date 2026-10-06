/* Public RTC (real-time clock) structure shared by the kernel and programs.
 *
 * The kernel's own rtc_time_t carries a different layout; this is the stable
 * user-facing shape returned by the SYS_RTC_TIME syscall. */
#ifndef INDEVOS_RTC_H
#define INDEVOS_RTC_H

#include <stdint.h>

typedef struct {
    uint16_t year;      /* full 4-digit year */
    uint8_t  month;     /* 1-12 */
    uint8_t  day;       /* 1-31 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  minute;    /* 0-59 */
    uint8_t  second;    /* 0-59 */
    uint8_t  weekday;   /* 0 = Sunday */
} indevos_rtc_time_t;

#endif /* INDEVOS_RTC_H */
