#pragma once

#include "vfs.h"

// Register the FAT32 filesystem with the VFS.
// Mounts the currently-mounted FAT32 filesystem at the given path (e.g. "/").
// The FAT32 filesystem must already be mounted via fat32_mount() before
// calling this function.
int vfs_fat32_register(const char *mount_path);

// Get the ops pointer (used by unmount logic to identify FAT32 mounts)
vfs_fs_ops_t *vfs_fat32_get_ops(void);
