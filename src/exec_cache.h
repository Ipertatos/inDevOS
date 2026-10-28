#pragma once
#include "types.h"

/* Exec image cache: whole-file copies of recently exec'd programs so a
 * fork+exec does not re-read a 1 MB busybox from disk every time.
 *
 *  - exec_image_load() behaves like vfs_read_file_alloc(path, out, 0): it
 *    returns the byte count and a private kalloc'd copy (NUL-terminated) that
 *    the caller kfree()s, or a negative errno.
 *  - Entries are validated on every hit (the file is opened; size, mtime and,
 *    where the fs has stat, inode/nlink must match) and invalidated by the VFS
 *    on open-for-write/truncate, write, ftruncate, unlink, rename, rmdir,
 *    mount and unmount, so FAT32, ext4 and tmpfs files stay coherent.
 *  - Bounded: total bytes <= min(16 MiB, RAM/32), LRU eviction, files with
 *    more than one hard link are never cached. */
int  exec_image_load(const char *path, void **out_buf);

/* VFS hooks (cheap when the cache is empty). */
void exec_cache_invalidate(const char *path);        /* exact path (case-insensitive) */
void exec_cache_invalidate_tree(const char *path);   /* path and everything below it */
void exec_cache_flush(void);

/* Statistics for `zeus` / tests. */
void exec_cache_stats(uint32_t *entries, uint64_t *bytes, uint64_t *hits, uint64_t *misses);
