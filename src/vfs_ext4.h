#pragma once

#include "vfs.h"

// Register the ext4 filesystem with the VFS.
// Mounts the currently-mounted ext4 filesystem at the given path (e.g. "/mnt").
// The ext4 filesystem must already be mounted via ext4_mount() before
// calling this function.
int vfs_ext4_register(const char *mount_path);

// Get the ops pointer (used by unmount logic to identify ext4 mounts)
vfs_fs_ops_t *vfs_ext4_get_ops(void);
