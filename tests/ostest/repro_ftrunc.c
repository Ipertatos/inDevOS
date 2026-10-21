/* repro_ftrunc: FAT32 truncate() always cut to 0 and ftruncate() ignored the
 * length (ostest fs.truncate).  Checks shrink, zero-filled grow, ftruncate,
 * and many shrink cycles (fsck of the image afterwards checks the FAT). */
#include "repro_common.h"
#include <sys/stat.h>
int main(void) {
    const char *f = "/tmp/rptr.bin"; static char z[10000], b[20000]; memset(z, 'x', sizeof z);
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); write(fd, z, sizeof z); close(fd);
    struct stat st;
    CHECK("ftrunc", "shrink", truncate(f, 3000) == 0 && stat(f, &st) == 0 && st.st_size == 3000, "size %lld errno=%d", (long long)st.st_size, errno);
    CHECK("ftrunc", "grow", truncate(f, 20000) == 0 && stat(f, &st) == 0 && st.st_size == 20000, "size %lld", (long long)st.st_size);
    fd = open(f, O_RDONLY); int n = 0, r; while ((r = read(fd, b + n, sizeof b - n)) > 0) n += r; close(fd);
    int bad = -1; for (int i = 0; i < 20000 && n == 20000; i++) if ((i < 3000 && b[i] != 'x') || (i >= 3000 && b[i])) { bad = i; break; }
    CHECK("ftrunc", "content", n == 20000 && bad < 0, "read %d, first bad byte at %d", n, bad);
    fd = open(f, O_RDWR); int fr = ftruncate(fd, 5); fstat(fd, &st); close(fd);
    CHECK("ftrunc", "ftruncate", fr == 0 && st.st_size == 5, "r=%d size %lld", fr, (long long)st.st_size);
    int ok = 1;
    for (int i = 0; i < 40 && ok; i++) {
        fd = open(f, O_WRONLY | O_TRUNC); write(fd, z, sizeof z); close(fd);
        if (truncate(f, 1000 + i * 100) || stat(f, &st) || st.st_size != 1000 + i * 100) ok = 0;
    }
    CHECK("ftrunc", "cycles", ok, "40 write/shrink cycles");
    unlink(f);
    RESULT("ftrunc");
}
