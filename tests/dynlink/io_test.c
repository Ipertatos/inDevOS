/* cat-style file I/O through the C library, one step at a time:
 * setlocale, fstat(stdout), open, fstat, posix_fadvise, aligned_alloc(4096, 128K), read
 * loop, write.  Prints the file then IO_OK, or IO_FAIL <step> <errno text> (buffer address
 * and st_blksize included) so a failure names the syscall that broke. */
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *step, int e, void *buf, long blk) {
    printf("IO_FAIL %s: %s (errno %d) buf=%p blksize=%ld\n", step, strerror(e), e, buf, blk);
    return 1;
}

int main(int argc, char **argv) {
    struct stat so, st; void *buf = NULL; long blk = -1;
    if (argc < 2) return fail("usage", EINVAL, buf, blk);
    setlocale(LC_ALL, "");
    if (fstat(1, &so) != 0) return fail("fstat_stdout", errno, buf, blk);
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) return fail("open", errno, buf, blk);
    if (fstat(fd, &st) != 0) return fail("fstat", errno, buf, blk);
    blk = (long)st.st_blksize;
    int a = posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (a != 0 && a != ESPIPE) return fail("fadvise", a, buf, blk);
    size_t sz = 128 * 1024;
    buf = aligned_alloc(4096, sz);
    if (!buf) return fail("aligned_alloc", errno, buf, blk);
    long total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sz);
        if (n < 0) return fail("read", errno, buf, blk);
        if (n == 0) break;
        if (write(1, buf, (size_t)n) != n) return fail("write", errno, buf, blk);
        total += n;
    }
    close(fd); free(buf);
    printf("IO_OK %ld bytes blksize=%ld\n", total, blk);
    return 0;
}
