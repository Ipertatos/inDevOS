/*
 * date - print the current wall-clock date and time.
 *
 * Real user program: reads the CMOS RTC through the SYS_RTC_TIME syscall
 * (gettimeofday stays on the monotonic uptime timebase).
 */
#include "syscalls.h"

static const char *const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    indevos_rtc_time_t t;
    if (rtc_time(&t) != 0) {
        printf("date: cannot read the real-time clock\n");
        return 1;
    }

    printf("%04u-%02u-%02u %02u:%02u:%02u",
           (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
           (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
    if (t.weekday < 7)
        printf(" (%s)", day_names[t.weekday]);
    printf("\n");
    return 0;
}
