/* fakemouse - user-mode fake pointer driver for inDevOS /dev/uinput.
 * Static musl.  Forks: the parent returns to the shell at once; the child
 * creates "inDevOS fakemouse" (UI_DEV_SETUP path), lists /dev and
 * /dev/input with readdir, checks fstat minors, waits [delay] seconds so an
 * evdev reader can attach, then moves the pointer round a 50x50 square,
 * ends with a (+40,+30) offset, clicks left, scrolls, presses right and
 * exits with it still held (the kernel must release it on exit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/input.h>
#include <linux/uinput.h>

static int ufd;
static void emit(int type, int code, int val) {
    struct input_event ie; memset(&ie, 0, sizeof(ie));
    ie.type = type; ie.code = code; ie.value = val;
    if (write(ufd, &ie, sizeof(ie)) != sizeof(ie)) printf("FAKEMOUSE_WRITE_ERR %d\n", errno);
}
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void move(int dx, int dy) { emit(EV_REL, REL_X, dx); emit(EV_REL, REL_Y, dy); emit(EV_SYN, SYN_REPORT, 0); msleep(15); }

static void list_dir(const char *p) {
    DIR *d = opendir(p);
    if (!d) { printf("FAKEMOUSE_LS %s ERR %d\n", p, errno); return; }
    printf("FAKEMOUSE_LS %s:", p);
    struct dirent *e;
    while ((e = readdir(d))) printf(" %s%s", e->d_name, e->d_type == DT_DIR ? "/" : e->d_type == DT_CHR ? "(c)" : "");
    printf("\n");
    closedir(d);
}

int main(int argc, char **argv) {
    int delay = argc > 1 ? atoi(argv[1]) : 3;
    pid_t pid = fork();
    if (pid < 0) { printf("FAKEMOUSE_FORK_ERR %d\n", errno); return 1; }
    if (pid > 0) { printf("FAKEMOUSE_BG pid=%d\n", (int)pid); return 0; }

    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { printf("FAKEMOUSE_OPEN_ERR %d\n", errno); return 1; }
    int ver = 0; ioctl(ufd, UI_GET_VERSION, &ver);
    int r = 0;
    r |= ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    r |= ioctl(ufd, UI_SET_EVBIT, EV_REL);
    r |= ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    r |= ioctl(ufd, UI_SET_KEYBIT, BTN_LEFT);
    r |= ioctl(ufd, UI_SET_KEYBIT, BTN_RIGHT);
    r |= ioctl(ufd, UI_SET_KEYBIT, BTN_MIDDLE);
    r |= ioctl(ufd, UI_SET_RELBIT, REL_X);
    r |= ioctl(ufd, UI_SET_RELBIT, REL_Y);
    r |= ioctl(ufd, UI_SET_RELBIT, REL_WHEEL);
    r |= ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    struct uinput_setup us; memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0x1234; us.id.product = 0x5678; us.id.version = 1;
    strcpy(us.name, "inDevOS fakemouse");
    r |= ioctl(ufd, UI_DEV_SETUP, &us);
    int cr = ioctl(ufd, UI_DEV_CREATE);
    char sys[32] = "?"; int sr = ioctl(ufd, UI_GET_SYSNAME(sizeof(sys)), sys);
    printf("FAKEMOUSE_CREATED ver=%d setbits=%d create=%d sysname=%s(%d)\n", ver, r, cr, sys, sr);
    if (cr) return 1;

    list_dir("/dev/input");
    list_dir("/dev");
    /* find our node by name, check fstat minor == 64+N */
    int mine = -1, minor_ok = 1;
    for (int i = 0; i < 16; i++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct stat st; fstat(fd, &st);
        struct stat sp; stat(p, &sp);
        if (minor(st.st_rdev) != 64 + i || major(st.st_rdev) != 13 || st.st_rdev != sp.st_rdev) minor_ok = 0;
        char name[80] = ""; ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (!strcmp(name, us.name)) mine = i;
        close(fd);
    }
    struct stat ust; fstat(ufd, &ust);
    printf("FAKEMOUSE_NODE event%d fstat_minors_ok=%d uinput_rdev=%d,%d\n", mine, minor_ok, major(ust.st_rdev), minor(ust.st_rdev));

    /* own evdev reader to read the events back */
    char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", mine);
    int rfd = open(p, O_RDONLY | O_NONBLOCK);
    fflush(stdout);
    sleep(delay);

    for (int side = 0; side < 4; side++)
        for (int s = 0; s < 10; s++)
            move(side == 0 ? 5 : side == 2 ? -5 : 0, side == 1 ? 5 : side == 3 ? -5 : 0);
    move(40, 30);
    emit(EV_KEY, BTN_LEFT, 1); emit(EV_SYN, SYN_REPORT, 0); msleep(30);
    emit(EV_KEY, BTN_LEFT, 0); emit(EV_SYN, SYN_REPORT, 0); msleep(30);
    emit(EV_REL, REL_WHEEL, 1); emit(EV_SYN, SYN_REPORT, 0);
    emit(EV_REL, REL_WHEEL, -1); emit(EV_SYN, SYN_REPORT, 0);
    emit(EV_KEY, BTN_RIGHT, 1); emit(EV_SYN, SYN_REPORT, 0);   /* left held on exit */

    long relx = 0, rely = 0, nrel = 0, nkey = 0, nsyn = 0;
    struct input_event ev[64]; ssize_t n;
    while (rfd >= 0 && (n = read(rfd, ev, sizeof(ev))) > 0)
        for (int i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
            if (ev[i].type == EV_REL) { nrel++; if (ev[i].code == REL_X) relx += ev[i].value; if (ev[i].code == REL_Y) rely += ev[i].value; }
            else if (ev[i].type == EV_KEY) nkey++;
            else if (ev[i].type == EV_SYN) nsyn++;
        }
    printf("FAKEMOUSE_READBACK rel=%ld key=%ld syn=%ld sum=(%ld,%ld)\n", nrel, nkey, nsyn, relx, rely);
    printf("FAKEMOUSE_DONE exiting with BTN_RIGHT held\n");
    fflush(stdout);
    return 0;                                   /* no UI_DEV_DESTROY: exit must clean up */
}
