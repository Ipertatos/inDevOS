#include "rtc.h"
#include "registers.h"
#include "serial.h"
#include "utils.h"
#include <stdint.h>

/* CMOS I/O ports */
#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

/* CMOS RTC register indices */
#define RTC_SECONDS    0x00
#define RTC_MINUTES    0x02
#define RTC_HOURS      0x04
#define RTC_DAY        0x07
#define RTC_MONTH      0x08
#define RTC_YEAR       0x09
#define RTC_CENTURY    0x32   /* may not be present on all chipsets */
#define RTC_STATUS_A   0x0A
#define RTC_STATUS_B   0x0B

/* Read a single CMOS register.  NMI disable bit (0x80) is left clear. */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

/* Convert BCD byte to binary */
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

/* Wait until the RTC "update in progress" flag is clear */
static void rtc_wait_ready(void) {
    /* Bit 7 of Status Register A = "Update in progress" */
    while (cmos_read(RTC_STATUS_A) & 0x80)
        ;
}

void rtc_read(rtc_time_t *t) {
    uint8_t status_b;
    uint8_t sec, min, hr, day, mon, yr, century;

    /* Read twice to avoid torn reads across an update cycle */
    uint8_t s1, s2;
    do {
        rtc_wait_ready();
        s1  = cmos_read(RTC_SECONDS);
        min = cmos_read(RTC_MINUTES);
        hr  = cmos_read(RTC_HOURS);
        day = cmos_read(RTC_DAY);
        mon = cmos_read(RTC_MONTH);
        yr  = cmos_read(RTC_YEAR);
        century = cmos_read(RTC_CENTURY);

        rtc_wait_ready();
        s2  = cmos_read(RTC_SECONDS);
    } while (s1 != s2);

    sec = s1;
    status_b = cmos_read(RTC_STATUS_B);

    /* Convert from BCD if needed (bit 2 of status B = binary mode) */
    if (!(status_b & 0x04)) {
        sec     = bcd_to_bin(sec);
        min     = bcd_to_bin(min);
        hr      = bcd_to_bin(hr & 0x7F);  /* mask AM/PM bit */
        day     = bcd_to_bin(day);
        mon     = bcd_to_bin(mon);
        yr      = bcd_to_bin(yr);
        century = bcd_to_bin(century);
    }

    /* Handle 12-hour mode (bit 1 of status B = 24-hour) */
    if (!(status_b & 0x02) && (hr & 0x80)) {
        hr = ((hr & 0x7F) + 12) % 24;
    }

    /* Compute full year */
    if (century > 0)
        t->year = (uint16_t)century * 100 + yr;
    else
        t->year = 2000 + yr;

    t->second = sec;
    t->minute = min;
    t->hour   = hr;
    t->day    = day;
    t->month  = mon;

    /* Compute day of week via Tomohiko Sakamoto's algorithm */
    {
        static const int dow_t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        int y = t->year;
        int m = t->month;
        if (m < 3) y--;
        t->weekday = (uint8_t)((y + y/4 - y/100 + y/400 + dow_t[m - 1] + t->day) % 7);
    }
}

/* Days in each month (non-leap) */
static const uint16_t days_before_month[] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static int is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint64_t rtc_unix_epoch(void) {
    rtc_time_t t;
    rtc_read(&t);

    /* Compute seconds since Unix epoch (1970-01-01 00:00:00 UTC) */
    uint64_t days = 0;

    /* Full years from 1970 to t.year-1 */
    for (int y = 1970; y < (int)t.year; y++) {
        days += is_leap(y) ? 366 : 365;
    }

    /* Months in current year */
    if (t.month >= 1 && t.month <= 12) {
        days += days_before_month[t.month - 1];
        if (t.month > 2 && is_leap(t.year))
            days++;
    }

    /* Days */
    days += t.day - 1;

    return days * 86400ULL + t.hour * 3600ULL + t.minute * 60ULL + t.second;
}

void rtc_print_time(void) {
    static const char *weekdays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    rtc_time_t t;
    rtc_read(&t);
    printf("%s %04u-%02u-%02u %02u:%02u:%02u UTC\n",
           weekdays[t.weekday], t.year, t.month, t.day,
           t.hour, t.minute, t.second);
}

void rtc_init(void) {
    rtc_time_t t;
    rtc_read(&t);
    printf("rtc: %04u-%02u-%02u %02u:%02u:%02u UTC\n",
           t.year, t.month, t.day,
           t.hour, t.minute, t.second);
    serial_printf("[rtc] Initialized: %04u-%02u-%02u %02u:%02u:%02u\n",
                  t.year, t.month, t.day, t.hour, t.minute, t.second);
}
