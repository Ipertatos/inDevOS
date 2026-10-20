/* faketablet - absolute uinput pointer (EV_ABS X/Y 0..1000, BTN_TOUCH).
 * Static musl, foreground.  Usage: faketablet [x y]   (0..1000 each)
 * Taps at (250,250), drags to (750,750), then rests at x,y (default 500,500)
 * and destroys the device.  The kernel logs pointer=(px,py) at destroy, which
 * must equal x*(W-1)/1000, y*(H-1)/1000. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>

static int ufd;
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void emit(int type, int code, int val) {
    struct input_event ie; memset(&ie, 0, sizeof(ie));
    ie.type = type; ie.code = code; ie.value = val;
    if (write(ufd, &ie, sizeof(ie)) != sizeof(ie)) printf("FAKETAB_WRITE_ERR %d\n", errno);
}
static void at(int x, int y) { emit(EV_ABS, ABS_X, x); emit(EV_ABS, ABS_Y, y); emit(EV_SYN, SYN_REPORT, 0); msleep(20); }

int main(int argc, char **argv) {
    int fx = argc > 2 ? atoi(argv[1]) : 500, fy = argc > 2 ? atoi(argv[2]) : 500;
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { printf("FAKETAB_OPEN_ERR %d\n", errno); return 1; }
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    ioctl(ufd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    struct uinput_abs_setup a; memset(&a, 0, sizeof(a));
    a.code = ABS_X; a.absinfo.minimum = 0; a.absinfo.maximum = 1000;
    int r1 = ioctl(ufd, UI_ABS_SETUP, &a);
    a.code = ABS_Y;
    int r2 = ioctl(ufd, UI_ABS_SETUP, &a);
    struct uinput_setup us; memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0x1234; us.id.product = 0x7ab1;
    strcpy(us.name, "inDevOS faketablet");
    ioctl(ufd, UI_DEV_SETUP, &us);
    int cr = ioctl(ufd, UI_DEV_CREATE);
    printf("FAKETAB_CREATED abs_setup=%d,%d create=%d target=(%d,%d)\n", r1, r2, cr, fx, fy);
    if (cr) return 1;
    msleep(100);
    at(250, 250);
    emit(EV_KEY, BTN_TOUCH, 1); emit(EV_SYN, SYN_REPORT, 0); msleep(30);
    for (int i = 1; i <= 10; i++) at(250 + i * 50, 250 + i * 50);
    emit(EV_KEY, BTN_TOUCH, 0); emit(EV_SYN, SYN_REPORT, 0); msleep(30);
    at(fx, fy);
    msleep(50);
    int dr = ioctl(ufd, UI_DEV_DESTROY);
    printf("FAKETAB_DONE destroy=%d\n", dr);
    close(ufd);
    return 0;
}
