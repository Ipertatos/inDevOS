/* Shared stat buffer layout (kernel copy_to_user / user stat()). */
#ifndef INDEVOS_STAT_H
#define INDEVOS_STAT_H

#include <stdint.h>

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
} stat_t;

#endif
