/*
 * kprof.h - tiny always-on cost counters (count, microseconds, sectors).
 *
 * Used to profile the in-OS installer: storage commands per device, FAT32
 * path lookups and directory scans, and the installer's own phases.
 * kprof_reset() clears them, kprof_dump() prints the non-zero ones to the
 * serial log. Storage lives in storage.c.
 */
#ifndef KPROF_H
#define KPROF_H
#include <stdint.h>
#include "ktime.h"

enum {
    KP_ST_RD_HW,        /* storage reads that reached the driver */
    KP_ST_RD_CACHED,    /* storage_read_sectors fully served by the block cache */
    KP_ST_WR_HW,        /* storage writes (driver) */
    KP_ST_FLUSH,        /* storage_flush */
    KP_FAT_RESOLVE,     /* fat32_resolve_path */
    KP_FAT_FIND,        /* fat32_find_in_dir */
    KP_FAT_DIRSCAN,     /* directory clusters read by lookups/slot searches */
    KP_FAT_CREATE,      /* fat32_create_file */
    KP_FAT_MKDIR,       /* fat32_create_directory */
    KP_FAT_OPEN,        /* fat32_open */
    KP_FAT_READ,        /* fat32_read */
    KP_FAT_WRITE,       /* fat32_write (data + dir entry update) */
    KP_FAT_UPDDIR,      /* fat32_update_dir_entry */
    KP_FAT_FATFLUSH,    /* fat32_flush_fat */
    KP_FAT_ALLOC,       /* fat32_allocate_cluster (count = clusters) */
    KP_FAT_TRUNC,       /* fat32_truncate_file */
    KP_VFS_EXISTS,      /* vfs_exists */
    KP_VFS_MKDIRP,      /* vfs_ensure_parent / vfs_mkdir_p */
    KP_INST_WALK,       /* installer: listing the live tree */
    KP_INST_READ,       /* installer: vfs_read_file_alloc per file */
    KP_INST_MKDIR,      /* installer: vfs_mkdir per directory */
    KP_INST_WRITE,      /* installer: vfs_write_file per file */
    KP_INST_LOG,        /* installer: console/log output */
    KP_INST_FINAL,      /* installer: final flush */
    KP_COUNT
};

typedef struct {
    uint64_t count;
    uint64_t us;
    uint64_t sectors;
} kprof_slot_t;

extern kprof_slot_t kprof_slots[KP_COUNT];
extern const char *const kprof_names[KP_COUNT];

static inline uint64_t kprof_begin(void) { return ktime_now_us(); }
static inline void kprof_end(int slot, uint64_t t0, uint64_t sectors) {
    kprof_slot_t *s = &kprof_slots[slot];
    s->count++;
    s->us += ktime_now_us() - t0;
    s->sectors += sectors;
}
static inline void kprof_add(int slot, uint64_t n) { kprof_slots[slot].count += n; }

void kprof_reset(void);
void kprof_dump(const char *tag);

#endif
