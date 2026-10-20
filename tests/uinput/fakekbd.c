/* fakekbd - user-mode fake keyboard driver for inDevOS /dev/uinput.
 * Static musl.  Uses the legacy struct uinput_user_dev write.  Forks; the
 * child waits [delay] seconds (so the shell is back at its prompt), types
 * "echo UINPUT_OK" + Enter, does a UI_DEV_DESTROY/CREATE cycle check, then
 * presses Left Shift and exits holding it (kernel must release it). */
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
    if (write(ufd, &ie, sizeof(ie)) != sizeof(ie)) printf("FAKEKBD_WRITE_ERR %d\n", errno);
}
static void key(int code, int down) { emit(EV_KEY, code, down); emit(EV_SYN, SYN_REPORT, 0); msleep(25); }
static void tap(int code, int shift) {
    if (shift) key(KEY_LEFTSHIFT, 1);
    key(code, 1); key(code, 0);
    if (shift) key(KEY_LEFTSHIFT, 0);
}
static int keycode(char c, int *shift) {
    static const char *row1 = "qwertyuiop", *row2 = "asdfghjkl", *row3 = "zxcvbnm";
    *shift = 0;
    if (c >= 'A' && c <= 'Z') { *shift = 1; c = (char)(c - 'A' + 'a'); }
    const char *p;
    if ((p = strchr(row1, c)) && c) return KEY_Q + (int)(p - row1);
    if ((p = strchr(row2, c)) && c) return KEY_A + (int)(p - row2);
    if ((p = strchr(row3, c)) && c) return KEY_Z + (int)(p - row3);
    if (c >= '1' && c <= '9') return KEY_1 + (c - '1');
    if (c == '0') return KEY_0;
    if (c == ' ') return KEY_SPACE;
    if (c == '_') { *shift = 1; return KEY_MINUS; }
    if (c == '-') return KEY_MINUS;
    if (c == '\n') return KEY_ENTER;
    return 0;
}

static int setup(void) {
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    for (int k = 1; k < 128; k++) ioctl(ufd, UI_SET_KEYBIT, k);
    struct uinput_user_dev ud; memset(&ud, 0, sizeof(ud));
    strcpy(ud.name, "inDevOS fakekbd");
    ud.id.bustype = BUS_VIRTUAL; ud.id.vendor = 0x1234; ud.id.product = 0x4b42; ud.id.version = 1;
    ssize_t w = write(ufd, &ud, sizeof(ud));
    int cr = ioctl(ufd, UI_DEV_CREATE);
    printf("FAKEKBD_CREATED legacy_write=%zd create=%d\n", w, cr);
    return cr;
}

int main(int argc, char **argv) {
    int delay = argc > 1 ? atoi(argv[1]) : 2;
    const char *text = argc > 2 ? argv[2] : "echo UINPUT_OK\n";
    pid_t pid = fork();
    if (pid < 0) { printf("FAKEKBD_FORK_ERR %d\n", errno); return 1; }
    if (pid > 0) { printf("FAKEKBD_BG pid=%d\n", (int)pid); return 0; }

    ufd = open("/dev/uinput", O_WRONLY);
    if (ufd < 0) { printf("FAKEKBD_OPEN_ERR %d\n", errno); return 1; }
    if (setup()) return 1;
    /* destroy + re-create on the same fd (UI_DEV_DESTROY path) */
    int dr = ioctl(ufd, UI_DEV_DESTROY);
    int cr2 = setup();
    char sys[32] = "?"; ioctl(ufd, UI_GET_SYSNAME(sizeof(sys)), sys);
    printf("FAKEKBD_RECREATE destroy=%d create=%d sysname=%s\n", dr, cr2, sys);
    fflush(stdout);
    sleep(delay);
    for (const char *c = text; *c; c++) {
        int sh, k = keycode(*c, &sh);
        if (k) tap(k, sh);
    }
    msleep(200);
    emit(EV_KEY, KEY_LEFTSHIFT, 1); emit(EV_SYN, SYN_REPORT, 0);
    printf("FAKEKBD_DONE exiting with LEFTSHIFT held\n");
    fflush(stdout);
    return 0;
}
