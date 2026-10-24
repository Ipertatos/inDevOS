/* dhcp - bring inDevOS networking up from a Linux shell (ash).
 *
 * Asks the kernel (private syscall 1000, op 1/2) to run the Zeus "dhcp" or
 * "ddhcp" command on its main loop, waits for it to finish and prints the
 * resulting address.
 *   dhcp       DHCP request (old config is kept if no server answers)
 *   dhcp -q    apply QEMU user-net defaults (10.0.2.15/24 gw 10.0.2.2)
 *   dhcp -a    only if no lease yet and not already tried this boot
 *   dhcp -s    show the current address
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/syscall.h>

#define SYS_INDEV_NET 1000

static long net(long op) { return syscall(SYS_INDEV_NET, op); }

static void show(long st) {
    unsigned ip = (unsigned)(st & 0xffffffffL);
    printf("ip %u.%u.%u.%u%s\n", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255,
           ip >> 24, ((st >> 32) & 1) ? " (dhcp lease)" : "");
}

int main(int argc, char **argv) {
    const char *a = argc > 1 ? argv[1] : "";
    long st = net(3);
    if (st < 0) { fprintf(stderr, "dhcp: kernel has no net helper (%ld)\n", st); return 1; }
    if (!strcmp(a, "-s")) { show(st); return 0; }
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
        puts("usage: dhcp [-q|-a|-s]"); return 0;
    }
    if (!strcmp(a, "-a")) {
        if ((st >> 32) & 1) { show(st); return 0; }
        if (access("/tmp/.dhcp_tried", F_OK) == 0) { show(st); return 0; }
        int fd = open("/tmp/.dhcp_tried", O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
    }
    long op = !strcmp(a, "-q") ? 2 : 1;
    unsigned gen0 = (unsigned)((st >> 40) & 0xffffff);
    printf("%s\n", op == 2 ? "dhcp: applying QEMU user-net defaults" :
                             "dhcp: requesting a lease...");
    fflush(stdout);
    if (net(op) < 0) { fprintf(stderr, "dhcp: request failed\n"); return 1; }
    for (int i = 0; i < 600; i++) {               /* up to 60 s */
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        st = net(3);
        if ((unsigned)((st >> 40) & 0xffffff) != gen0) {
            show(st);
            return (op == 1 && !((st >> 32) & 1)) ? 2 : 0;
        }
    }
    fprintf(stderr, "dhcp: timed out waiting for the kernel\n");
    return 3;
}
