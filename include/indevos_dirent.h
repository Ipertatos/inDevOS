#ifndef INDEVOS_DIRENT_H
#define INDEVOS_DIRENT_H

/*
 * Public directory-entry layout for SYS_READDIR.
 *
 * This used to live only in src/syscall.h (a kernel header), so user programs
 * had no way to call readdir() without pulling in kernel internals.  It is the
 * single definition shared by the kernel and by user space now.
 */

#include <stdint.h>

#define INDEVOS_NAME_MAX 256

/* d_type values */
#define INDEVOS_DT_UNKNOWN 0
#define INDEVOS_DT_FILE    1
#define INDEVOS_DT_DIR     2

typedef struct {
    char     d_name[INDEVOS_NAME_MAX];
    uint32_t d_ino;
    uint8_t  d_type;
    uint8_t  d_pad[3];
    uint32_t d_size;
} indevos_dirent_t;

#endif /* INDEVOS_DIRENT_H */
