#include "vfs_fat32.h"
#include "vfs.h"
#include "fat32.h"
#include "string.h"
#include "utils.h"
#include "mem.h"

// ---- FAT32 adapter for VFS --------------------------------------------------
//
// Translates VFS calls into fat32_* calls.  The VFS handle (cookie) is
// simply a pointer to a fat32_file_t returned by fat32_open().

static int fat32_vfs_read_only(vfs_filesystem_t *fs) {
    (void)fs;
    return fat32_medium_read_only();
}

static void* fat32_vfs_open(vfs_filesystem_t *fs, const char *rel_path,
                            uint32_t flags) {
    (void)fs;
    const char *mode = (flags & VFS_O_WRITE) ? "w" : "r";

    // Build an absolute path for the FAT32 layer (it expects "/..." or plain name)
    char full[VFS_MAX_PATH];
    if (rel_path[0] == '\0') return NULL;

    full[0] = '/';
    size_t rlen = strlen(rel_path);
    if (rlen >= VFS_MAX_PATH - 1) rlen = VFS_MAX_PATH - 2;
    memmove(full + 1, rel_path, rlen);
    full[1 + rlen] = '\0';

    /* O_TRUNC: FAT32 has no truncate operation, so release the file's cluster
     * chain explicitly.  Without this a shorter file copied over a longer one
     * kept the old size and its stale tail - `cp small.txt big.txt` produced a
     * corrupt file.  Directories are refused by fat32_truncate_file(). */
    if ((flags & VFS_O_TRUNC) && fat32_file_exists(full)) {
        fat32_truncate_file(full);
    }

    fat32_file_t *f = fat32_open(full, mode);

    /* A directory must not be handed back as a file handle: sys_open() falls
     * through to its readdir probe (and open_directory_fd) only when the file
     * open fails, so returning a file fd here made `ls <subdir>` fail with
     * "not a directory" while `ls /` worked (the root has no such handle). */
    if (f && (f->attributes & FAT32_ATTR_DIRECTORY)) {
        fat32_close(f);
        return NULL;
    }
    return (void *)f;
}

static int fat32_vfs_close(vfs_filesystem_t *fs, void *handle) {
    (void)fs;
    return fat32_close((fat32_file_t *)handle);
}

static int fat32_vfs_read(vfs_filesystem_t *fs, void *handle,
                          void *buffer, uint32_t size) {
    (void)fs;
    return fat32_read((fat32_file_t *)handle, buffer, size);
}

static int fat32_vfs_write(vfs_filesystem_t *fs, void *handle,
                           const void *buffer, uint32_t size) {
    (void)fs;
    return fat32_write((fat32_file_t *)handle, buffer, size);
}

static int fat32_vfs_get_size(vfs_filesystem_t *fs, void *handle) {
    (void)fs;
    fat32_file_t *f = (fat32_file_t *)handle;
    if (!f || !f->in_use) return -1;
    return (int)f->file_size;
}

static int fat32_vfs_seek(vfs_filesystem_t *fs, void *handle,
                          int32_t offset, int whence) {
    (void)fs;
    fat32_file_t *f = (fat32_file_t *)handle;
    if (!f) return -1;

    uint32_t new_pos = 0;
    switch (whence) {
        case VFS_SEEK_SET: new_pos = (uint32_t)offset; break;
        case VFS_SEEK_CUR: new_pos = f->file_position + offset; break;
        case VFS_SEEK_END: new_pos = f->file_size + offset; break;
        default: return -1;
    }
    if (fat32_seek(f, new_pos) < 0) return -1;
    return (int)f->file_position;
}

static int fat32_vfs_readdir(vfs_filesystem_t *fs, const char *rel_path,
                             vfs_dirent_t *entries, uint32_t max) {
    (void)fs;

    // Build path
    char full[VFS_MAX_PATH];
    if (rel_path[0] == '\0') {
        full[0] = '/';
        full[1] = '\0';
    } else {
        full[0] = '/';
        size_t rlen = strlen(rel_path);
        if (rlen >= VFS_MAX_PATH - 1) rlen = VFS_MAX_PATH - 2;
        memmove(full + 1, rel_path, rlen);
        full[1 + rlen] = '\0';
    }

    // Use a temporary array of fat32_dir_list_t
    fat32_dir_list_t *fat_entries = (fat32_dir_list_t *)kalloc(
        max * sizeof(fat32_dir_list_t));
    if (!fat_entries) return -1;

    int count = fat32_list_directory(full, fat_entries, max);
    if (count < 0) {
        kfree(fat_entries);
        return -1;
    }

    // Convert to vfs_dirent_t
    for (int i = 0; i < count; i++) {
        size_t nlen = strlen(fat_entries[i].name);
        if (nlen >= VFS_MAX_NAME) nlen = VFS_MAX_NAME - 1;
        memmove(entries[i].name, fat_entries[i].name, nlen);
        entries[i].name[nlen] = '\0';
        entries[i].inode = 0;  // FAT32 doesn't have inodes
        entries[i].type  = fat_entries[i].is_directory ? VFS_DIRECTORY : VFS_FILE;
        entries[i].size  = fat_entries[i].size;
    }

    kfree(fat_entries);
    return count;
}

static int fat32_vfs_create(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    full[0] = '/';
    size_t rlen = strlen(rel_path);
    if (rlen >= VFS_MAX_PATH - 1) rlen = VFS_MAX_PATH - 2;
    memmove(full + 1, rel_path, rlen);
    full[1 + rlen] = '\0';
    return fat32_create_file(full);
}

static int fat32_vfs_mkdir(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    full[0] = '/';
    size_t rlen = strlen(rel_path);
    if (rlen >= VFS_MAX_PATH - 1) rlen = VFS_MAX_PATH - 2;
    memmove(full + 1, rel_path, rlen);
    full[1 + rlen] = '\0';
    return fat32_create_directory(full);
}

static int fat32_vfs_unlink(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    full[0] = '/';
    size_t rlen = strlen(rel_path);
    if (rlen >= VFS_MAX_PATH - 1) rlen = VFS_MAX_PATH - 2;
    memmove(full + 1, rel_path, rlen);
    full[1 + rlen] = '\0';
    return fat32_delete_file(full);
}

static int fat32_vfs_exists(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    full[0] = '/';
    size_t rlen = strlen(rel_path);
    if (rlen >= VFS_MAX_PATH - 1) rlen = VFS_MAX_PATH - 2;
    memmove(full + 1, rel_path, rlen);
    full[1 + rlen] = '\0';
    return fat32_file_exists(full);
}

// ---- Static ops table -------------------------------------------------------

static vfs_fs_ops_t fat32_vfs_ops = {
    .open    = fat32_vfs_open,
    .close   = fat32_vfs_close,
    .read    = fat32_vfs_read,
    .write   = fat32_vfs_write,
    .seek    = fat32_vfs_seek,
    .get_size = fat32_vfs_get_size,
    .readdir = fat32_vfs_readdir,
    .create  = fat32_vfs_create,
    .mkdir   = fat32_vfs_mkdir,
    .unlink  = fat32_vfs_unlink,
    .exists  = fat32_vfs_exists,
    .read_only = fat32_vfs_read_only,
};

// ---- Public -----------------------------------------------------------------

int vfs_fat32_register(const char *mount_path) {
    if (!fat32_is_mounted()) {
        printf("vfs_fat32: FAT32 not mounted – cannot register with VFS\n");
        return -1;
    }
    return vfs_mount(mount_path, &fat32_vfs_ops, &fat32_filesystem);
}

vfs_fs_ops_t *vfs_fat32_get_ops(void) {
    return &fat32_vfs_ops;
}
