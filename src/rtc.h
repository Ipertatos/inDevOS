#pragma once

#include <stdint.h>

/* Date/time structure returned by the RTC driver */
typedef struct {
    uint8_t  second;
    uint8_t  minute;
    uint8_t  hour;       /* 24-hour format */
    uint8_t  day;        /* day of month (1-31) */
    uint8_t  month;      /* 1-12 */
    uint16_t year;       /* full 4-digit year */
    uint8_t  weekday;    /* 0 = Sunday (computed) */
} rtc_time_t;

/* Initialise the RTC driver (reads initial time) */
void rtc_init(void);

/* Read current date/time from the CMOS RTC */
void rtc_read(rtc_time_t *t);

/* Return seconds since midnight 1 Jan 2000 (simple epoch) */
uint64_t rtc_unix_epoch(void);

/* Print the current date/time to the console */
void rtc_print_time(void);
