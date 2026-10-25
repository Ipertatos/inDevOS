/**
 * @file inotify.h
 * @brief Linux-compatible inotify: path-based watches fed by VFS hooks.
 */
#ifndef INOTIFY_H
#define INOTIFY_H

#include "types.h"

#define FD_TYPE_INOTIFY 16

#define IN_ACCESS        0x00000001u
#define IN_MODIFY        0x00000002u
#define IN_ATTRIB        0x00000004u
#define IN_CLOSE_WRITE   0x00000008u
#define IN_CLOSE_NOWRITE 0x00000010u
#define IN_OPEN          0x00000020u
#define IN_MOVED_FROM    0x00000040u
#define IN_MOVED_TO      0x00000080u
#define IN_CREATE        0x00000100u
#define IN_DELETE        0x00000200u
#define IN_DELETE_SELF   0x00000400u
#define IN_MOVE_SELF     0x00000800u
#define IN_UNMOUNT       0x00002000u
#define IN_Q_OVERFLOW    0x00004000u
#define IN_IGNORED       0x00008000u
#define IN_ONLYDIR       0x01000000u
#define IN_DONT_FOLLOW   0x02000000u
#define IN_EXCL_UNLINK   0x04000000u
#define IN_MASK_CREATE   0x10000000u
#define IN_MASK_ADD      0x20000000u
#define IN_ISDIR         0x40000000u
#define IN_ONESHOT       0x80000000u
#define IN_ALL_EVENTS    0x00000fffu

/* Nonzero while any inotify instance exists: VFS hooks return at once
 * otherwise, so the hot paths pay one load. */
extern volatile int inotify_active;

void    *inotify_create(void);
void     inotify_release(void *h);
/* abspath must be absolute; is_dir from the caller's lookup. */
int      inotify_add(void *h, const char *abspath, uint32_t mask, bool is_dir);
int      inotify_rm(void *h, int wd);
/* Copies whole events into kbuf.  0 = queue empty, -EINVAL = next event
 * does not fit. */
long     inotify_take(void *h, void *kbuf, size_t len);
bool     inotify_ready(void *h);
uint32_t inotify_gen(void *h);
uint32_t inotify_pending_bytes(void *h);

/* VFS hooks (call without the VFS lock held). */
void inotify_vfs_event(const char *path, uint32_t mask);
void inotify_vfs_rename(const char *oldpath, const char *newpath, bool is_dir);

#endif
