/* Declarations for inDevOS freestanding DOOM (INDVOS); avoids host unistd/sys/stat mismatches. */
#ifndef DOOM_INDEVOS_SHIM_H
#define DOOM_INDEVOS_SHIM_H

#include <stddef.h>
#include <indevos_stat.h>

#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
#endif

typedef long off_t;

int access(const char *pathname, int mode);
/* open() comes from <fcntl.h> (variadic); libc_shim implements the kernel ABI. */
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int fstat(int fd, stat_t *buf);
void *alloca(size_t size);
int mkdir(const char *pathname, int mode);

#endif
