#pragma once

#include "types.h"

// ---- VFS: Virtual File System abstraction layer ----------------------------
//
// The VFS allows multiple filesystem implementations to coexist.  Each
// filesystem registers a set of operations.  Mount points bind a filesystem
// instance to a prefix in the path namespace (e.g. "/", "/dev", "/proc").
//
// File handles returned by vfs_open() are valid until vfs_close() is called.

#define VFS_MAX_MOUNTPOINTS 32   /* was 8: ext4 test fixture + scratch copies need more */
#define VFS_MAX_OPEN_FILES  64
#define VFS_MAX_PATH        1024
#define VFS_MAX_NAME        256
/* Upper bound on the entries one directory listing returns (sys_readdir,
 * getdents64, the installer). Callers allocate it on the heap. */
#define VFS_READDIR_MAX     2048
/* Listings start at VFS_READDIR_MAX entries and double until the directory
 * fits (vfs_readdir_alloc), up to this bound. */
#define VFS_READDIR_LIMIT   262144

// Node types
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x03
#define VFS_BLOCKDEVICE 0x04
#define VFS_PIPE        0x05
#define VFS_SYMLINK     0x06

// Open flags
#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_CREATE 0x04
#define VFS_O_TRUNC  0x08
#define VFS_O_APPEND 0x10

// Seek whence
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

// Forward declarations
struct vfs_node;
struct vfs_dirent;
struct vfs_filesystem;

// Directory entry (returned by readdir)
typedef struct vfs_dirent {
    char     name[VFS_MAX_NAME];
    uint32_t inode;        // filesystem-specific identifier
    uint8_t  type;         // VFS_FILE, VFS_DIRECTORY, etc.
    uint8_t  lnk;          // 1: the entry is a symlink (type = its target's type, VFS_FILE if dangling)
    uint32_t size;
} vfs_dirent_t;

// Filesystem operations – each filesystem implementation fills these in
typedef struct vfs_stat {
    uint32_t mode, uid, gid, nlink, blksize;
    uint64_t size, ino, blocks;
    int64_t  atime, mtime, ctime;        /* seconds since the epoch */
} vfs_stat_t;

typedef struct vfs_fs_ops {
    // Open a file relative to the mountpoint.  'rel_path' has the mount
    // prefix stripped.  Returns a filesystem-private handle (cookie).
    void* (*open)(struct vfs_filesystem *fs, const char *rel_path, uint32_t flags);

    // Close a previously opened handle
    int   (*close)(struct vfs_filesystem *fs, void *handle);

    // Read up to 'size' bytes into 'buffer'.  Returns bytes read, 0 for EOF, <0 on error.
    int   (*read)(struct vfs_filesystem *fs, void *handle, void *buffer, uint32_t size);

    // Write 'size' bytes from 'buffer'.  Returns bytes written or <0 on error.
    int   (*write)(struct vfs_filesystem *fs, void *handle, const void *buffer, uint32_t size);

    // Seek to 'offset' from 'whence'.  Returns new absolute position or <0.
    int   (*seek)(struct vfs_filesystem *fs, void *handle, int32_t offset, int whence);

    /* Optional: return file size without a SEEK_END walk. */
    int   (*get_size)(struct vfs_filesystem *fs, void *handle);

    // Read next directory entry.  Returns 0 on success, -1 at end.
    int   (*readdir)(struct vfs_filesystem *fs, const char *rel_path,
                     vfs_dirent_t *entries, uint32_t max_entries);

    // Create a regular file.  Returns 0 on success.
    int   (*create)(struct vfs_filesystem *fs, const char *rel_path);

    // Create a directory.  Returns 0 on success.
    int   (*mkdir)(struct vfs_filesystem *fs, const char *rel_path);

    // Delete a file or empty directory.  Returns 0 on success.
    int   (*unlink)(struct vfs_filesystem *fs, const char *rel_path);

    // Check if a path exists.  Returns 1 if yes, 0 if no.
    int   (*exists)(struct vfs_filesystem *fs, const char *rel_path);

    /** Optional char-device ioctl; NULL makes vfs_ioctl return -ENOTTY */
    int   (*ioctl)(struct vfs_filesystem *fs, void *handle, unsigned long cmd, void *arg);

    /* Optional (NULL = old FAT32 behaviour in vfs.c). Paths are relative to
     * the mount; rename gets both paths on the same filesystem. */
    int   (*rename)(struct vfs_filesystem *fs, const char *old_rel, const char *new_rel);
    int   (*rmdir)(struct vfs_filesystem *fs, const char *rel_path);
    int   (*truncate)(struct vfs_filesystem *fs, const char *rel_path, uint64_t len);
    int   (*ftruncate)(struct vfs_filesystem *fs, void *handle, uint64_t len);
    int   (*sync)(struct vfs_filesystem *fs);
    /* Optional: real inode metadata (follows symlinks).  0 or -errno. */
    int   (*stat)(struct vfs_filesystem *fs, const char *rel_path, struct vfs_stat *st);
    /* Optional: nonzero when the medium is read-only; the VFS then refuses
     * every modification with -EROFS before calling the filesystem. */
    int   (*read_only)(struct vfs_filesystem *fs);
    /* Optional symlink / hard link / attribute ops.  None of them follows a
     * symlink in the final component (the VFS resolver, vfs_resolve(), has
     * already expanded links in the earlier components).
     * readlink: target length (no NUL) or -EINVAL (not a link) / -ENOENT.
     * lstat:    like stat, but a final symlink reports S_IFLNK itself.
     * setattr:  VFS_SA_MODE sets the low 12 mode bits, VFS_SA_OWNER uid/gid
     *           ((uint32_t)-1 = leave unchanged).  0 or -errno. */
    int   (*readlink)(struct vfs_filesystem *fs, const char *rel_path, char *buf, uint32_t cap);
    int   (*symlink)(struct vfs_filesystem *fs, const char *rel_path, const char *target);
    int   (*link)(struct vfs_filesystem *fs, const char *old_rel, const char *new_rel);
    int   (*lstat)(struct vfs_filesystem *fs, const char *rel_path, struct vfs_stat *st);
    int   (*setattr)(struct vfs_filesystem *fs, const char *rel_path, int what,
                     uint32_t mode, uint32_t uid, uint32_t gid);
} vfs_fs_ops_t;
#define VFS_SA_MODE  1
#define VFS_SA_QUIET 0x100   /* vfs_setattr: no IN_ATTRIB (create-time mode) */
#define VFS_SA_OWNER 2
#define VFS_SYMLOOP_MAX 40

// A mounted filesystem instance
typedef struct vfs_filesystem {
    char           mount_path[VFS_MAX_PATH]; // e.g. "/" or "/dev"
    uint32_t       mount_path_len;
    vfs_fs_ops_t  *ops;
    void          *fs_data;                  // filesystem-private data (e.g. fat32_fs_t*)
    bool           mounted;
} vfs_filesystem_t;

// VFS file descriptor – wraps a filesystem-private handle
typedef struct vfs_file {
    bool              in_use;
    vfs_filesystem_t *fs;           // which filesystem owns this file
    void             *fs_handle;    // filesystem-private handle (cookie)
    uint32_t          flags;        // open flags
    char              path[VFS_MAX_PATH];
} vfs_file_t;

// ---- Public API ------------------------------------------------------------

// Initialize the VFS subsystem
void vfs_init(void);

// Mount a filesystem at 'path'.  'ops' and 'fs_data' are owned by the caller
// and must remain valid for the lifetime of the mount.
int  vfs_mount(const char *path, vfs_fs_ops_t *ops, void *fs_data);

// Unmount the filesystem at 'path'.
int  vfs_unmount(const char *path);

// File operations
int  vfs_open(const char *path, uint32_t flags);   // returns fd or -1
int  vfs_close(int fd);
int  vfs_read(int fd, void *buffer, uint32_t size);
int  vfs_write(int fd, const void *buffer, uint32_t size);
int  vfs_seek(int fd, int32_t offset, int whence);
/** ioctl on an open VFS fd (optional per-filesystem). Returns 0 on success, negative errno. */
int  vfs_ioctl(int fd, unsigned long cmd, void *arg);

// Directory operations
int  vfs_readdir(const char *path, vfs_dirent_t *entries, uint32_t max);
/* Whole listing of @path in a kalloc'd array (*out, caller kfree()s; @extra
 * spare slots at the end).  Returns the entry count or <0 (then *out is NULL). */
int  vfs_readdir_alloc(const char *path, vfs_dirent_t **out, uint32_t extra);
/* Listing of an open directory, cached per open handle @key: copies entries
 * [pos, pos+max) into @out and returns how many (0 at the end, <0 on error).
 * pos == 0 (opendir / rewinddir) rebuilds the listing; @augment may append up
 * to @extra synthetic entries (returns the new count). */
typedef int (*vfs_dir_augment_fn)(const char *path, vfs_dirent_t *ents, int total, int cap);
int  vfs_dircache_read(const void *key, const char *path, uint32_t pos, vfs_dirent_t *out, uint32_t max,
                       vfs_dir_augment_fn augment, uint32_t extra);
/* Forget the cached listing of handle @key (directory fd closed). */
void vfs_dircache_drop(const void *key);
int  vfs_create(const char *path);
int  vfs_mkdir(const char *path);
/** Create every missing component of 'path' (mkdir -p). Returns 0 or -errno. */
int  vfs_mkdir_p(const char *path);
/** Create the parent directory of 'path' (mkdir -p of dirname). */
int  vfs_ensure_parent(const char *path);
/** Create a normal Unix-like tree (/bin, /home/user/Documents, …) if missing. */
void vfs_seed_standard_layout(void);
int  vfs_unlink(const char *path);
int  vfs_rename(const char *oldpath, const char *newpath);
int  vfs_rmdir(const char *path);
int  vfs_truncate(const char *path, uint64_t len);
/* Filesystem-provided metadata; -ENOSYS when the fs has no stat op. */
int vfs_stat(const char *path, vfs_stat_t *st);
/* Symlinks (filesystems with a readlink op: tmpfs, ext2/3/4).
 * vfs_resolve: normalise absolute @path (".", "..", "//") into @out, expanding
 * every symlink on the way (and the last component too when @follow_last).
 * ".." after a link is physical, like Linux.  -ELOOP after VFS_SYMLOOP_MAX
 * expansions, -ENAMETOOLONG, 0 on success (missing components are kept). */
int vfs_resolve(const char *path, char *out, size_t outsz, bool follow_last);
/* Target of the symlink @path (not followed): length, -EINVAL if not a link. */
int vfs_readlink(const char *path, char *buf, uint32_t cap);
/* lstat: -ENOSYS when the fs has neither lstat nor stat. */
int vfs_lstat(const char *path, vfs_stat_t *st);
/* Create symlink @path -> @target / hard link @newpath -> @oldpath.
 * -EPERM when the filesystem has no such op (FAT32, exFAT), -EXDEV across mounts. */
int vfs_symlink(const char *target, const char *path);
int vfs_link(const char *oldpath, const char *newpath);
/* chmod/chown stored in the inode: -ENOSYS when the fs has no setattr op. */
int vfs_setattr(const char *path, int what, uint32_t mode, uint32_t uid, uint32_t gid);
/* true when @path lives on a filesystem with real permission bits (setattr op) */
bool vfs_has_inode_perms(const char *path);
int  vfs_ftruncate(int fd, uint64_t len);
int  vfs_sync_all(void);
int  vfs_exists(const char *path);

/** Copy open file's path into buf (for fchmod/fstat). Returns 0 or -1. */
int  vfs_fd_get_path(int fd, char *buf, size_t buflen);

// Utility
vfs_filesystem_t* vfs_find_mount(const char *path, const char **rel_path_out);

// Check if any filesystem is mounted at "/" (or any mount exists)
int vfs_has_root_mount(void);

// Whole-file helpers (see vfs.c). max_size of 0 means "no limit".
int vfs_read_file_alloc(const char *path, void **out_buf, uint32_t max_size);
int vfs_write_file(const char *path, const void *buf, uint32_t size);
