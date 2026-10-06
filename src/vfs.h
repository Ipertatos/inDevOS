#pragma once

#include "types.h"

// ---- VFS: Virtual File System abstraction layer ----------------------------
//
// The VFS allows multiple filesystem implementations to coexist.  Each
// filesystem registers a set of operations.  Mount points bind a filesystem
// instance to a prefix in the path namespace (e.g. "/", "/dev", "/proc").
//
// File handles returned by vfs_open() are valid until vfs_close() is called.

#define VFS_MAX_MOUNTPOINTS 8
#define VFS_MAX_OPEN_FILES  64
#define VFS_MAX_PATH        1024
#define VFS_MAX_NAME        256

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
    uint32_t size;
} vfs_dirent_t;

// Filesystem operations – each filesystem implementation fills these in
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
} vfs_fs_ops_t;

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
