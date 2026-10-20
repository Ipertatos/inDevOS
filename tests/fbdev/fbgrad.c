/* fbgrad - Linux fbdev smoke test: query /dev/fb0, draw a gradient via mmap,
 * exercise lseek/write/read, KDSETMODE, then restore and exit. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(int argc, char **argv) {
    int hold = argc > 1 ? atoi(argv[1]) : 4;
    int tty = open("/dev/tty0", O_RDWR);
    CHECK(tty >= 0, "open /dev/tty0 -> %d", tty);
    if (tty >= 0) {
        int m = -1;
        CHECK(ioctl(tty, KDGETMODE, &m) == 0 && m == KD_TEXT, "KDGETMODE text (%d)", m);
        struct vt_stat st;
        CHECK(ioctl(tty, VT_GETSTATE, &st) == 0, "VT_GETSTATE active=%d", st.v_active);
        struct vt_mode vm;
        CHECK(ioctl(tty, VT_GETMODE, &vm) == 0, "VT_GETMODE");
        CHECK(ioctl(tty, KDSETMODE, KD_GRAPHICS) == 0, "KDSETMODE KD_GRAPHICS");
    }
    int fd = open("/dev/fb0", O_RDWR);
    CHECK(fd >= 0, "open /dev/fb0 -> %d", fd);
    if (fd < 0) return 1;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    CHECK(ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0, "VSCREENINFO %ux%u virt %ux%u bpp %u R%u/%u G%u/%u B%u/%u",
          v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel,
          v.red.offset, v.red.length, v.green.offset, v.green.length, v.blue.offset, v.blue.length);
    CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0, "FSCREENINFO id='%.16s' smem_start=%#lx smem_len=%u line_length=%u type=%u visual=%u",
          f.id, f.smem_start, f.smem_len, f.line_length, f.type, f.visual);
    CHECK(f.type == FB_TYPE_PACKED_PIXELS && f.visual == FB_VISUAL_TRUECOLOR, "packed truecolor");
    struct fb_var_screeninfo v2 = v;
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &v2) == 0, "PUT_VSCREENINFO same mode accepted");
    v2 = v; v2.xres = 640; v2.yres = 480; v2.xres_virtual = 640; v2.yres_virtual = 480;
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &v2) < 0, "PUT_VSCREENINFO 640x480 rejected");
    v2 = v; v2.xoffset = 0; v2.yoffset = 0;
    CHECK(ioctl(fd, FBIOPAN_DISPLAY, &v2) == 0, "PAN_DISPLAY 0,0");
    CHECK(ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) == 0, "BLANK unblank");

    uint8_t *p = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "mmap %u bytes -> %p", f.smem_len, (void *)p);
    if (p == MAP_FAILED) return 1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t y = 0; y < v.yres; y++) {
        uint32_t *row = (uint32_t *)(p + (size_t)y * f.line_length);
        for (uint32_t x = 0; x < v.xres; x++) {
            uint32_t r = x * 255 / (v.xres - 1), g = y * 255 / (v.yres - 1), b = 255 - r;
            row[x] = (r << v.red.offset) | (g << v.green.offset) | (b << v.blue.offset);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    printf("info gradient %ux%u drawn via mmap in %ld us\n", v.xres, v.yres, us);
    ioctl(fd, FBIOPAN_DISPLAY, &v2);

    /* write(): white 200px bar on row 20..39, read() it back */
    uint32_t bar[200];
    for (int i = 0; i < 200; i++) bar[i] = 0xFFFFFFu;
    int wok = 1;
    for (int y = 20; y < 40; y++) {
        off_t o = (off_t)y * f.line_length + 20 * 4;
        if (lseek(fd, o, SEEK_SET) != o || write(fd, bar, sizeof(bar)) != (ssize_t)sizeof(bar)) wok = 0;
    }
    CHECK(wok, "lseek+write white bar");
    uint32_t back[200] = {0};
    lseek(fd, (off_t)30 * f.line_length + 20 * 4, SEEK_SET);
    CHECK(read(fd, back, sizeof(back)) == (ssize_t)sizeof(back) && (back[0] & 0xFFFFFF) == 0xFFFFFF, "read back %#x", back[0]);
    CHECK(lseek(fd, 0, SEEK_END) == (off_t)f.smem_len, "lseek SEEK_END = smem_len");
    CHECK(((uint32_t *)(p + 30 * f.line_length))[100] == back[80], "mmap view matches write()");
    ioctl(fd, FBIOPAN_DISPLAY, &v2);
    printf("FBGRAD %s (%d failures), holding %d s\n", fails ? "FAILED" : "PASSED", fails, hold);
    fflush(stdout);
    sleep(hold);
    munmap(p, f.smem_len);
    close(fd);
    if (tty >= 0) { ioctl(tty, KDSETMODE, KD_TEXT); close(tty); }
    printf("FBGRAD_DONE\n");
    return fails ? 1 : 0;
}
