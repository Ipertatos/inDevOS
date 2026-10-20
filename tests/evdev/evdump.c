/* evdump - minimal evtest-like checker for inDevOS /dev/input/eventN.
 * Static musl.  Usage: evdump [seconds]   (default 12)
 * Lists every device (EVIOCGNAME/ID/BIT/VERSION), checks O_NONBLOCK reads
 * return EAGAIN, then polls all devices and prints each event. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
static int test_bit(int n, const unsigned long *a) { return (a[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 12;
    struct pollfd pf[16]; int minor[16]; int n = 0;
    long counts[16][4]; memset(counts, 0, sizeof(counts));
    for (int i = 0; i < 16; i++) {
        char path[32]; snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[80] = "?"; int ver = 0; struct input_id id = {0};
        unsigned long ev[NBITS(EV_MAX + 1)] = {0}, key[NBITS(KEY_MAX + 1)] = {0}, rel[NBITS(REL_MAX + 1)] = {0};
        ioctl(fd, EVIOCGVERSION, &ver);
        ioctl(fd, EVIOCGID, &id);
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        int evb = ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
        int nkeys = 0; for (int k = 0; k <= KEY_MAX; k++) nkeys += test_bit(k, key);
        char buf[24]; ssize_t r = read(fd, buf, sizeof(buf));
        int eagain = r < 0 && errno == EAGAIN;
        struct input_absinfo ai; int absr = ioctl(fd, EVIOCGABS(ABS_X), &ai);
        printf("EVDUMP_DEV event%d \"%s\" ver=%x bus=%04x vendor=%04x product=%04x evbits=%lx(%d) keys=%d rel=%s%s%s btnleft=%d nonblock_eagain=%d gabs=%d\n",
               i, name, ver, id.bustype, id.vendor, id.product, ev[0], evb, nkeys,
               test_bit(REL_X, rel) ? "X" : "", test_bit(REL_Y, rel) ? "Y" : "",
               test_bit(REL_WHEEL, rel) ? "W" : "", test_bit(BTN_LEFT, key), eagain, absr);
        if (!strstr(name, "keyboard") && !strstr(name, "Keyboard") && test_bit(BTN_LEFT, key)) {
            int g = ioctl(fd, EVIOCGRAB, (void *)1);
            int g2 = ioctl(fd, EVIOCGRAB, (void *)0);
            printf("EVDUMP_GRAB event%d grab=%d ungrab=%d\n", i, g, g2);
        }
        pf[n].fd = fd; pf[n].events = POLLIN; minor[n] = i; n++;
    }
    printf("EVDUMP_DEVICES %d\n", n);
    fflush(stdout);
    if (!n) return 1;
    time_t end = time(NULL) + secs;
    long total = 0;
    while (time(NULL) < end) {
        int pr = poll(pf, n, 500);
        if (pr <= 0) continue;
        for (int j = 0; j < n; j++) {
            if (pf[j].fd >= 0 && (pf[j].revents & (POLLHUP | POLLERR))) {
                printf("EVDUMP_HUP event%d\n", minor[j]);   /* device removed */
                close(pf[j].fd); pf[j].fd = -1;
                continue;
            }
            if (!(pf[j].revents & POLLIN)) continue;
            struct input_event evs[16];
            ssize_t r = read(pf[j].fd, evs, sizeof(evs));
            if (r <= 0) continue;
            for (int e = 0; e < r / (ssize_t)sizeof(evs[0]); e++) {
                struct input_event *x = &evs[e];
                if (x->type == EV_KEY) counts[minor[j]][0]++;
                else if (x->type == EV_REL) counts[minor[j]][1]++;
                else if (x->type == EV_SYN) counts[minor[j]][2]++;
                else counts[minor[j]][3]++;
                total++;
                if (x->type != EV_SYN || total < 400)
                    printf("EVDUMP_EV event%d t=%ld.%06ld type=%d code=%d value=%d\n", minor[j],
                           (long)x->input_event_sec, (long)x->input_event_usec, x->type, x->code, x->value);
            }
        }
        fflush(stdout);
    }
    for (int j = 0; j < n; j++)
        printf("EVDUMP_SUM event%d key=%ld rel=%ld syn=%ld other=%ld\n", minor[j],
               counts[minor[j]][0], counts[minor[j]][1], counts[minor[j]][2], counts[minor[j]][3]);
    printf("EVDUMP_DONE total=%ld\n", total);
    return 0;
}
