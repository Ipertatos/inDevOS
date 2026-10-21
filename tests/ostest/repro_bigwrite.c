/* repro_bigwrite: 1 MiB chunk writes to /tmp failed at 2 MiB with EPERM
 * (a bare -1 from the FAT32 driver).  Writes 6 MiB and reports the first
 * failing offset/errno, then reads it back. */
#include "repro_common.h"
int main(void) {
    const char *f = "/tmp/rpbig.bin"; size_t ch = 1 << 20, sz = 6 << 20;
    unsigned char *b = malloc(ch); int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK("bigwrite", "open", fd >= 0, "fd=%d errno=%d", fd, errno);
    size_t o; int bad = 0;
    for (o = 0; o < sz && fd >= 0; o += ch) {
        memset(b, (int)(o >> 20) + 1, ch); size_t w = 0;
        while (w < ch) { ssize_t r = write(fd, b + w, ch - w); if (r <= 0) { bad = r < 0 ? errno : -1; break; } w += r; }
        if (bad) break;
    }
    CHECK("bigwrite", "write", !bad, "first failure at %zu errno=%d (%s)", o, bad, bad > 0 ? strerror(bad) : "short");
    if (fd >= 0) close(fd);
    fd = open(f, O_RDONLY); size_t tot = 0; ssize_t r; int mism = 0;
    while (fd >= 0 && (r = read(fd, b, ch)) > 0) { for (ssize_t i = 0; i < r; i++) if (b[i] != (unsigned char)(((tot + i) >> 20) + 1)) { mism++; break; } tot += r; }
    if (fd >= 0) close(fd);
    CHECK("bigwrite", "readback", tot == (bad ? o : sz) && !mism, "read %zu bytes, mismatch=%d", tot, mism);
    unlink(f); free(b);
    RESULT("bigwrite");
}
