#include "vfs.h"
#include "kprof.h"
#include "string.h"
#include "utils.h"
#include "mem.h"
#include "atomic.h"
#include "../include/indevos_errno.h"
#ifndef EXDEV
#define EXDEV 18
#endif

// Use kernel's strncmp
#define strncmp kstrncmp

/*
 * VFS subsystem lock.
 *
 * Previous approach disabled interrupts globally (irq_save/restore) which
 * caused latency spikes and interrupt starvation during long I/O.
 *
 * New approach: a klock, which disables preemption but not IRQs.  All FS
 * operations (FAT32 and ext4 alike) are serialized here, and interrupts
 * can still fire during long I/O.
 */
static spinlock_t vfs_lock = {0};
#define VFS_LOCK()   do { klock(&vfs_lock); } while(0)
#define VFS_UNLOCK() do { kunlock(&vfs_lock); } while(0)

// ---- VFS global state -------------------------------------------------------

static vfs_filesystem_t mount_table[VFS_MAX_MOUNTPOINTS];
static vfs_file_t       open_files[VFS_MAX_OPEN_FILES];

// ---- Initialization ---------------------------------------------------------

void vfs_init(void) {
    memset(mount_table, 0, sizeof(mount_table));
    memset(open_files,  0, sizeof(open_files));
    printf("vfs: Virtual filesystem initialized\n");
}

// ---- Mount / Unmount --------------------------------------------------------

int vfs_mount(const char *path, vfs_fs_ops_t *ops, void *fs_data) {
    VFS_LOCK();
    if (!path || !ops) { VFS_UNLOCK(); return -EINVAL; }

    // Normalise path: ensure it starts with '/'
    if (path[0] != '/') {
        printf("vfs: mount path must start with '/'\n");
        VFS_UNLOCK(); return -EINVAL;
    }

    // Check for duplicates
    for (int i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        if (mount_table[i].mounted && strEql(mount_table[i].mount_path, path)) {
            printf("vfs: '%s' already mounted\n", path);
            VFS_UNLOCK(); return -EBUSY;
        }
    }

    // Find a free slot
    for (int i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        if (!mount_table[i].mounted) {
            size_t plen = strlen(path);
            if (plen >= VFS_MAX_PATH) plen = VFS_MAX_PATH - 1;
            memmove(mount_table[i].mount_path, path, plen);
            mount_table[i].mount_path[plen] = '\0';

            // Remove trailing slash (except for root "/")
            if (plen > 1 && mount_table[i].mount_path[plen - 1] == '/') {
                mount_table[i].mount_path[plen - 1] = '\0';
                plen--;
            }

            mount_table[i].mount_path_len = (uint32_t)plen;
            mount_table[i].ops     = ops;
            mount_table[i].fs_data = fs_data;
            mount_table[i].mounted = true;

            int used = 0;
            for (int k = 0; k < VFS_MAX_MOUNTPOINTS; k++) if (mount_table[k].mounted) used++;
            printf("vfs: Mounted filesystem at '%s' (%d of %d mount slots used)\n", mount_table[i].mount_path, used, VFS_MAX_MOUNTPOINTS);
            VFS_UNLOCK(); return 0;
        }
    }

    printf("vfs: No free mount slots (%d in use)\n", VFS_MAX_MOUNTPOINTS);
    VFS_UNLOCK(); return -ENOMEM;
}

/* Close an open file; caller must already hold vfs_lock. */
static int vfs_close_locked(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !open_files[fd].in_use) return -EBADF;

    vfs_file_t *vf = &open_files[fd];
    int ret = 0;
    if (vf->fs && vf->fs->ops && vf->fs->ops->close) {
        ret = vf->fs->ops->close(vf->fs, vf->fs_handle);
    }
    vf->in_use = false;
    vf->fs_handle = NULL;
    return ret;
}

int vfs_unmount(const char *path) {
    VFS_LOCK();
    if (!path) { VFS_UNLOCK(); return -EINVAL; }

    for (int i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        if (mount_table[i].mounted && strEql(mount_table[i].mount_path, path)) {
            // Close any open files belonging to this mount.
            // Must use the _locked variant: vfs_close() re-takes the
            // non-recursive vfs_lock and would deadlock here.
            for (int j = 0; j < VFS_MAX_OPEN_FILES; j++) {
                if (open_files[j].in_use && open_files[j].fs == &mount_table[i]) {
                    vfs_close_locked(j);
                }
            }
            mount_table[i].mounted = false;
            printf("vfs: Unmounted '%s'\n", path);
            VFS_UNLOCK(); return 0;
        }
    }
    printf("vfs: '%s' not mounted\n", path);
    VFS_UNLOCK(); return -ENOENT;
}

// ---- Path resolution --------------------------------------------------------

/**
 * Find the longest-matching mount point for 'path'.
 * Sets *rel_path_out to the portion of the path after the mount prefix.
 * Returns NULL if no mount matches.
 */
vfs_filesystem_t* vfs_find_mount(const char *path, const char **rel_path_out) {
    if (!path || path[0] != '/') return NULL;

    vfs_filesystem_t *best = NULL;
    uint32_t best_len = 0;

    for (int i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        vfs_filesystem_t *fs = &mount_table[i];
        if (!fs->mounted) continue;

        uint32_t mlen = fs->mount_path_len;

        // Root "/" matches everything
        if (mlen == 1 && fs->mount_path[0] == '/') {
            if (best_len < 1) {
                best = fs;
                best_len = 1;
            }
            continue;
        }

        // Check if path starts with the mount prefix
        if (strncmp(path, fs->mount_path, mlen) == 0) {
            // The character after the prefix must be '/' or '\0'
            char after = path[mlen];
            if (after == '/' || after == '\0') {
                if (mlen > best_len) {
                    best = fs;
                    best_len = mlen;
                }
            }
        }
    }

    if (best && rel_path_out) {
        const char *rel = path + best_len;
        // Skip leading slash of relative portion
        if (*rel == '/') rel++;
        *rel_path_out = rel;
    }

    return best;
}

// ---- File operations --------------------------------------------------------

int vfs_open(const char *path, uint32_t flags) {
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (!fs || !fs->ops || !fs->ops->open) {
        printf("vfs: No filesystem for '%s'\n", path);
        VFS_UNLOCK(); return -ENODEV;
    }

    // Find free file descriptor
    int fd = -1;
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        printf("vfs: No free file descriptors\n");
        VFS_UNLOCK(); return -ENFILE;
    }

    // Auto-create if O_CREATE and the fs supports it
    uint32_t open_flags = flags;
    if ((flags & VFS_O_CREATE) && fs->ops->create) {
        if (!fs->ops->exists || !fs->ops->exists(fs, rel)) {
            /* A file created just now is empty: O_TRUNC has nothing to do,
             * and on FAT32 it cost another lookup and a directory write. */
            if (fs->ops->create(fs, rel) == 0 && fs->ops->exists)
                open_flags &= ~VFS_O_TRUNC;
        }
    }

    void *handle = fs->ops->open(fs, rel, open_flags);
    if (!handle) {
        VFS_UNLOCK(); return -ENOENT;
    }

    open_files[fd].in_use    = true;
    open_files[fd].fs        = fs;
    open_files[fd].fs_handle = handle;
    open_files[fd].flags     = flags;
    size_t plen = strlen(path);
    if (plen >= VFS_MAX_PATH) plen = VFS_MAX_PATH - 1;
    memmove(open_files[fd].path, path, plen);
    open_files[fd].path[plen] = '\0';

    VFS_UNLOCK(); return fd;
}

int vfs_close(int fd) {
    VFS_LOCK();
    int ret = vfs_close_locked(fd);
    VFS_UNLOCK(); return ret;
}

int vfs_read(int fd, void *buffer, uint32_t size) {
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !open_files[fd].in_use) { VFS_UNLOCK(); return -EBADF; }

    vfs_file_t *vf = &open_files[fd];
    if (!vf->fs || !vf->fs->ops || !vf->fs->ops->read) { VFS_UNLOCK(); return -EINVAL; }
    int ret = vf->fs->ops->read(vf->fs, vf->fs_handle, buffer, size);
    VFS_UNLOCK(); return ret;
}

int vfs_write(int fd, const void *buffer, uint32_t size) {
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !open_files[fd].in_use) { VFS_UNLOCK(); return -EBADF; }

    vfs_file_t *vf = &open_files[fd];
    if (!(vf->flags & VFS_O_WRITE)) { VFS_UNLOCK(); return -EBADF; }  // not opened for writing
    if (!vf->fs || !vf->fs->ops || !vf->fs->ops->write) { VFS_UNLOCK(); return -EINVAL; }
    int ret = vf->fs->ops->write(vf->fs, vf->fs_handle, buffer, size);
    VFS_UNLOCK(); return ret;
}

int vfs_seek(int fd, int32_t offset, int whence) {
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !open_files[fd].in_use) { VFS_UNLOCK(); return -EBADF; }

    vfs_file_t *vf = &open_files[fd];
    if (!vf->fs || !vf->fs->ops || !vf->fs->ops->seek) { VFS_UNLOCK(); return -EINVAL; }
    int ret = vf->fs->ops->seek(vf->fs, vf->fs_handle, offset, whence);
    VFS_UNLOCK(); return ret;
}

int vfs_ioctl(int fd, unsigned long cmd, void *arg) {
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !open_files[fd].in_use) {
        VFS_UNLOCK();
        return -EBADF;
    }
    vfs_file_t *vf = &open_files[fd];
    if (!vf->fs || !vf->fs->ops || !vf->fs->ops->ioctl) {
        VFS_UNLOCK();
        return -ENOTTY;
    }
    int ret = vf->fs->ops->ioctl(vf->fs, vf->fs_handle, cmd, arg);
    VFS_UNLOCK();
    return ret;
}

// ---- Directory operations ---------------------------------------------------

int vfs_readdir_alloc(const char *path, vfs_dirent_t **out, uint32_t extra) {
    *out = NULL;
    for (uint32_t cap = VFS_READDIR_MAX; ; cap *= 2) {
        vfs_dirent_t *e = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * (cap + extra));
        if (!e) return -ENOMEM;
        int n = vfs_readdir(path, e, cap);
        if (n < 0) { kfree(e); return n; }
        if ((uint32_t)n < cap || cap >= VFS_READDIR_LIMIT) { *out = e; return n; }
        kfree(e);                                   /* may be truncated: retry bigger */
    }
}

/* Per-open-directory listing cache.  Slots are keyed by the directory fd's
 * path handle (shared by dup/fork) and checked against the path, so a stale
 * slot can never serve another directory; every read at position 0 rebuilds. */
#define VFS_DIRCACHE_SLOTS 32
typedef struct {
    const void   *key;
    char         *path;
    vfs_dirent_t *ents;
    int           total;
    uint64_t      stamp;
} vfs_dircache_t;
static vfs_dircache_t dircache[VFS_DIRCACHE_SLOTS];
static spinlock_t dircache_lock = {0};
static uint64_t dircache_clock;

static int dircache_copy(vfs_dircache_t *c, uint32_t pos, vfs_dirent_t *out, uint32_t max) {
    if (pos >= (uint32_t)c->total) return 0;
    uint32_t n = (uint32_t)c->total - pos;
    if (n > max) n = max;
    memcpy(out, c->ents + pos, sizeof(vfs_dirent_t) * n);
    c->stamp = ++dircache_clock;
    return (int)n;
}

int vfs_dircache_read(const void *key, const char *path, uint32_t pos, vfs_dirent_t *out, uint32_t max,
                      vfs_dir_augment_fn augment, uint32_t extra) {
    if (pos > 0) {
        spinlock_lock(&dircache_lock);
        for (int i = 0; i < VFS_DIRCACHE_SLOTS; i++) {
            vfs_dircache_t *c = &dircache[i];
            if (c->key == key && c->path && strEql(c->path, path)) {
                int r = dircache_copy(c, pos, out, max);
                spinlock_unlock(&dircache_lock);
                return r;
            }
        }
        spinlock_unlock(&dircache_lock);
    }
    /* (re)build outside the lock */
    vfs_dirent_t *e = NULL;
    int n = vfs_readdir_alloc(path, &e, augment ? extra : 0);
    if (augment) {
        if (!e && !(e = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * (extra ? extra : 1)))) return -ENOMEM;
        n = augment(path, e, n, (n > 0 ? n : 0) + (int)extra);
    }
    if (n < 0) { if (e) kfree(e); return n; }
    size_t pl = strlen(path);
    char *pc = (char *)kalloc(pl + 1);
    if (!pc) { kfree(e); return -ENOMEM; }
    memcpy(pc, path, pl + 1);
    vfs_dirent_t *old_e = NULL; char *old_p = NULL;
    spinlock_lock(&dircache_lock);
    int slot = -1;
    for (int i = 0; i < VFS_DIRCACHE_SLOTS && slot < 0; i++) if (dircache[i].key == key) slot = i;
    for (int i = 0; i < VFS_DIRCACHE_SLOTS && slot < 0; i++) if (!dircache[i].key) slot = i;
    if (slot < 0) {                                 /* evict the least recently used */
        slot = 0;
        for (int i = 1; i < VFS_DIRCACHE_SLOTS; i++) if (dircache[i].stamp < dircache[slot].stamp) slot = i;
    }
    vfs_dircache_t *c = &dircache[slot];
    old_e = c->ents; old_p = c->path;
    c->key = key; c->path = pc; c->ents = e; c->total = n;
    int r = dircache_copy(c, pos, out, max);
    spinlock_unlock(&dircache_lock);
    if (old_e) kfree(old_e);
    if (old_p) kfree(old_p);
    return r;
}

void vfs_dircache_drop(const void *key) {
    for (int i = 0; i < VFS_DIRCACHE_SLOTS; i++) {
        vfs_dirent_t *e = NULL; char *p = NULL;
        spinlock_lock(&dircache_lock);
        if (dircache[i].key == key) {
            e = dircache[i].ents; p = dircache[i].path;
            dircache[i].key = NULL; dircache[i].ents = NULL; dircache[i].path = NULL; dircache[i].total = 0;
        }
        spinlock_unlock(&dircache_lock);
        if (e) kfree(e);
        if (p) kfree(p);
    }
}

int vfs_readdir(const char *path, vfs_dirent_t *entries, uint32_t max) {
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    int ret = (fs && fs->ops && fs->ops->readdir) ? fs->ops->readdir(fs, rel, entries, max)
                                                   : -ENODEV;
    /* Show mount points that live directly under @path (e.g. /mnt/ventoy
     * in /mnt), even if the parent filesystem has no such directory. */
    int n = ret > 0 ? ret : 0, added = 0;
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') pl--;
    for (int i = 0; i < VFS_MAX_MOUNTPOINTS && (uint32_t)n < max; i++) {
        vfs_filesystem_t *m = &mount_table[i];
        if (!m->mounted || m->mount_path_len <= 1) continue;
        int last = (int)m->mount_path_len - 1;
        while (last > 0 && m->mount_path[last] != '/') last--;
        size_t parent_len = last == 0 ? 1 : (size_t)last;
        if (parent_len != pl || strncmp(m->mount_path, path, pl) != 0) continue;
        const char *nm = m->mount_path + last + 1;
        bool dup = false;
        for (int k = 0; k < n; k++) if (strEql(entries[k].name, nm)) { dup = true; break; }
        if (dup) continue;
        memset(&entries[n], 0, sizeof(entries[n]));
        size_t l = strlen(nm);
        if (l >= VFS_MAX_NAME) l = VFS_MAX_NAME - 1;
        memmove(entries[n].name, nm, l);
        entries[n].type = VFS_DIRECTORY;
        n++; added++;
    }
    if (ret < 0 && added == 0) { VFS_UNLOCK(); return ret; }
    VFS_UNLOCK(); return n;
}

int vfs_create(const char *path) {
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (!fs || !fs->ops || !fs->ops->create) { VFS_UNLOCK(); return -ENODEV; }
    int ret = fs->ops->create(fs, rel);
    VFS_UNLOCK(); return ret;
}

int vfs_mkdir(const char *path) {
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (!fs || !fs->ops || !fs->ops->mkdir) { VFS_UNLOCK(); return -ENODEV; }
    int ret = fs->ops->mkdir(fs, rel);
    VFS_UNLOCK(); return ret;
}

int vfs_mkdir_p(const char *path) {
    if (!path || path[0] == '\0') return -EINVAL;
    if (path[0] == '/' && path[1] == '\0') return 0;
    if (!vfs_has_root_mount()) return -ENODEV;

    char buf[VFS_MAX_PATH];
    size_t n = 0;
    const char *p = path;
    if (*p == '/') {
        buf[n++] = '/';
        p++;
    }
    while (*p == '/') p++;

    while (*p) {
        if (n > 1) {
            if (n + 1 >= VFS_MAX_PATH) return -ENAMETOOLONG;
            buf[n++] = '/';
        }
        while (*p && *p != '/') {
            if (n + 1 >= VFS_MAX_PATH) return -ENAMETOOLONG;
            buf[n++] = *p++;
        }
        buf[n] = '\0';
        if (!vfs_exists(buf)) {
            int rc = vfs_mkdir(buf);
            if (rc < 0 && !vfs_exists(buf))
                return rc;
        }
        while (*p == '/') p++;
    }
    return 0;
}

static int vfs_ensure_parent_impl(const char *path);
int vfs_ensure_parent(const char *path) {
    uint64_t kp_t0 = kprof_begin();
    int r = vfs_ensure_parent_impl(path);
    kprof_end(KP_VFS_MKDIRP, kp_t0, 0);
    return r;
}

static int vfs_ensure_parent_impl(const char *path) {
    if (!path || path[0] == '\0') return -EINVAL;
    char buf[VFS_MAX_PATH];
    size_t n = 0;
    while (path[n] && n + 1 < VFS_MAX_PATH) {
        buf[n] = path[n];
        n++;
    }
    buf[n] = '\0';
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = '\0';
    while (n > 0 && buf[n - 1] != '/')
        n--;
    if (n <= 1)
        return 0;
    buf[n - 1] = '\0';
    if (buf[0] == '\0')
        return 0;
    return vfs_mkdir_p(buf);
}

void vfs_seed_standard_layout(void) {
    static const char *dirs[] = {
        "/bin",
        "/etc",
        "/tmp",
        "/apps",
        "/os",
        "/os/apps",
        "/home",
        "/home/user",
        "/home/user/Documents",
        "/home/user/Desktop",
        "/home/user/Downloads",
        "/Desktop",
        NULL
    };
    if (!vfs_has_root_mount())
        return;
    for (int i = 0; dirs[i]; i++)
        vfs_mkdir_p(dirs[i]);

    {
        extern int tmpfs_mount_tmp(void);
        tmpfs_mount_tmp();
    }

    if (!vfs_exists("/home/user/Documents/readme.txt")) {
        static const char msg[] =
            "Welcome to inDevOS.\n"
            "\n"
            "Your files live under /home/user.\n"
            "  Documents   notes and text files\n"
            "  Desktop     icons shown on the desktop\n"
            "  Downloads   incoming files\n"
            "\n"
            "Programs are in /bin. Temporary files go in /tmp.\n";
        vfs_write_file("/home/user/Documents/readme.txt", msg,
                       (uint32_t)(sizeof(msg) - 1));
    }
}

int vfs_unlink(const char *path) {
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (!fs || !fs->ops || !fs->ops->unlink) { VFS_UNLOCK(); return -ENODEV; }
    int ret = fs->ops->unlink(fs, rel);
    VFS_UNLOCK(); return ret;
}

int vfs_exists(const char *path) {
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (!fs || !fs->ops || !fs->ops->exists) { VFS_UNLOCK(); return 0; }
    uint64_t kp_t0 = kprof_begin();
    int ret = fs->ops->exists(fs, rel);
    kprof_end(KP_VFS_EXISTS, kp_t0, 0);
    VFS_UNLOCK(); return ret;
}

int vfs_fd_get_path(int fd, char *buf, size_t buflen) {
    if (!buf || buflen == 0 || fd < 0 || fd >= VFS_MAX_OPEN_FILES)
        return -EINVAL;
    VFS_LOCK();
    if (!open_files[fd].in_use) {
        VFS_UNLOCK();
        return -EBADF;
    }
    size_t plen = strlen(open_files[fd].path);
    if (plen + 1 >= buflen) {
        VFS_UNLOCK();
        return -ENAMETOOLONG;
    }
    memcpy(buf, open_files[fd].path, plen + 1);
    VFS_UNLOCK();
    return 0;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EINVAL;
    if (!vfs_has_root_mount()) return -ENODEV;
    {
        VFS_LOCK();
        const char *r1 = NULL, *r2 = NULL;
        vfs_filesystem_t *f1 = vfs_find_mount(oldpath, &r1);
        vfs_filesystem_t *f2 = vfs_find_mount(newpath, &r2);
        if (f1 != f2) { VFS_UNLOCK(); return -EXDEV; }   /* mv falls back to copy */
        if (f1 && f1->ops && f1->ops->rename) {
            int rr = f1->ops->rename(f1, r1, r2);
            VFS_UNLOCK(); return rr;
        }
        VFS_UNLOCK();
    }
    /* Same-filesystem rename only for now (single FAT32 root mount). */
    extern int fat32_rename(const char *oldpath, const char *newpath);
    int r = fat32_rename(oldpath, newpath);
    if (r == 0) return 0;
    if (r == -ENOENT || r == -ENOTEMPTY || r == -EIO || r == -EINVAL)
        return r;
    return r < 0 ? r : -EIO;
}

int vfs_rmdir(const char *path) {
    if (!path) return -EINVAL;
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (fs && fs->ops && fs->ops->rmdir) { int r = fs->ops->rmdir(fs, rel); VFS_UNLOCK(); return r; }
    VFS_UNLOCK();
    extern int fat32_remove_empty_directory(const char *path);
    return fat32_remove_empty_directory(path);               /* FAT32: unchanged */
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    if (!path || !st) return -EINVAL;
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (!fs || !fs->ops || !fs->ops->stat) { VFS_UNLOCK(); return -ENOSYS; }
    int r = fs->ops->stat(fs, rel ? rel : "", st);
    VFS_UNLOCK();
    return r;
}

int vfs_truncate(const char *path, uint64_t len) {
    if (!path) return -EINVAL;
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (fs && fs->ops && fs->ops->truncate) { int r = fs->ops->truncate(fs, rel, len); VFS_UNLOCK(); return r; }
    VFS_UNLOCK();
    extern int fat32_truncate_to(const char *path, uint32_t len);
    if (len > 0xFFFFFFFFull) return -EINVAL;                 /* FAT32 4 GiB limit */
    int fr = fat32_truncate_to(path, (uint32_t)len);
    return fr == 0 ? 0 : (fr < -1 ? fr : -EIO);
}

int vfs_ftruncate(int fd, uint64_t len) {
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !open_files[fd].in_use) { VFS_UNLOCK(); return -EBADF; }
    vfs_file_t *vf = &open_files[fd];
    int r = 0;
    extern vfs_fs_ops_t *vfs_fat32_get_ops(void);
    extern int fat32_truncate_handle_v(void *f, uint32_t len);
    if (vf->fs && vf->fs->ops && vf->fs->ops->ftruncate)
        r = vf->fs->ops->ftruncate(vf->fs, vf->fs_handle, len);
    else if (vf->fs && vf->fs->ops == vfs_fat32_get_ops())  /* FAT32 */
        r = len > 0xFFFFFFFFull ? -EINVAL : fat32_truncate_handle_v(vf->fs_handle, (uint32_t)len);
    VFS_UNLOCK(); return r;
}

int vfs_sync_all(void) {
    VFS_LOCK();
    int r = 0;
    for (int i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        vfs_filesystem_t *m = &mount_table[i];
        if (m->mounted && m->ops && m->ops->sync && m->ops->sync(m) != 0) r = -EIO;
    }
    VFS_UNLOCK(); return r;
}

int vfs_has_root_mount(void) {
    VFS_LOCK();
    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount("/", &rel);
    int has = (fs != NULL);
    VFS_UNLOCK();
    return has;
}

// ---- Whole-file convenience helpers -----------------------------------------

/* Read an entire file into a kalloc'd, NUL-terminated buffer.
 * On success returns the file size (>= 0) and stores the buffer in *out_buf
 * (caller frees with kfree).  Returns negative errno on failure. */
int vfs_read_file_alloc(const char *path, void **out_buf, uint32_t max_size) {
    if (!path || !out_buf) return -EINVAL;
    *out_buf = NULL;

    extern void exec_dbg_mark(const char *phase);
    exec_dbg_mark("read_file_alloc: open");
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return fd;
    exec_dbg_mark("read_file_alloc: size");

    int size = -1;
    VFS_LOCK();
    if (fd >= 0 && fd < VFS_MAX_OPEN_FILES && open_files[fd].in_use) {
        vfs_file_t *vf = &open_files[fd];
        if (vf->fs && vf->fs->ops && vf->fs->ops->get_size)
            size = vf->fs->ops->get_size(vf->fs, vf->fs_handle);
    }
    VFS_UNLOCK();
    if (size < 0) {
        size = vfs_seek(fd, 0, VFS_SEEK_END);
        if (size < 0) { vfs_close(fd); return -EIO; }
        vfs_seek(fd, 0, VFS_SEEK_SET);
    }
    if (max_size && (uint32_t)size > max_size) { vfs_close(fd); return -EFBIG; }

    uint8_t *buf = (uint8_t *)kalloc((uint32_t)size + 1);
    if (!buf) { vfs_close(fd); return -ENOMEM; }
    exec_dbg_mark("read_file_alloc: buffer allocated");

    uint32_t got = 0;
    while (got < (uint32_t)size) {
        int n = vfs_read(fd, buf + got, (uint32_t)size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    exec_dbg_mark("read_file_alloc: bytes read");
    vfs_close(fd);
    buf[got] = '\0';
    *out_buf = buf;
    return (int)got;
}

/* Create/overwrite a file with the given contents.
 * Returns 0 on success, negative errno on failure. */
int vfs_write_file(const char *path, const void *buf, uint32_t size) {
    if (!path || (!buf && size)) return -EINVAL;

    vfs_ensure_parent(path);
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return fd;

    uint32_t written = 0;
    while (written < size) {
        int n = vfs_write(fd, (const uint8_t *)buf + written, size - written);
        if (n <= 0) { vfs_close(fd); return -EIO; }
        written += (uint32_t)n;
    }
    return vfs_close(fd);
}
