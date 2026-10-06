#include "vfs_ext4.h"
#include "vfs.h"
#include "ext4.h"
#include "ext2.h"
#include "string.h"
#include "utils.h"
#include "mem.h"

#define EXT4_VFS_BEGIN() ext4_enter()
#define EXT4_VFS_END()   ext4_leave()

// ---- Ext4 VFS adapter -------------------------------------------------------
//
// Translates VFS calls into ext4_* calls.  Unlike FAT32, ext4 has no
// persistent open-file handle, so we maintain a small per-open cookie
// (ext4_vfs_handle_t) that tracks the inode, current position, and size.

#define EXT4_VFS_MAX_HANDLES 64

typedef struct {
    uint32_t inode;
    uint32_t position;
    uint32_t size;
    bool     in_use;
} ext4_vfs_handle_t;

static ext4_vfs_handle_t ext4_handles[EXT4_VFS_MAX_HANDLES];

static void ext4_build_full_path(char *full, const char *rel_path) {
    if (rel_path[0] == '\0') {
        full[0] = '/';
        full[1] = '\0';
        return;
    }
    full[0] = '/';
    size_t rlen = strlen(rel_path);
    if (rlen >= VFS_MAX_PATH - 1) rlen = VFS_MAX_PATH - 2;
    memmove(full + 1, rel_path, rlen);
    full[1 + rlen] = '\0';
}

static void* ext4_vfs_open(vfs_filesystem_t *fs, const char *rel_path,
                           uint32_t flags) {
    (void)fs;
    if (!rel_path || rel_path[0] == '\0') return NULL;

    char full[VFS_MAX_PATH];
    ext4_build_full_path(full, rel_path);

    EXT4_VFS_BEGIN();
    uint32_t ino = ext4_lookup_path(full);
    if (ino == 0) { EXT4_VFS_END(); return NULL; }

    ext2_inode_t inode;
    if (ext4_read_inode(ino, &inode) != 0) { EXT4_VFS_END(); return NULL; }

    if ((inode.i_mode & EXT2_S_IFDIR)) { EXT4_VFS_END(); return NULL; }

    int slot = -1;
    for (int i = 0; i < EXT4_VFS_MAX_HANDLES; i++) {
        if (!ext4_handles[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { EXT4_VFS_END(); return NULL; }

    uint32_t file_size = inode.i_size;
    if (inode.i_dir_acl) {
        file_size |= ((uint64_t)inode.i_dir_acl << 32);
    }

    if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE)) {
        ext4_write_file(ino, NULL, 0, 0);
        file_size = 0;
    }

    ext4_handles[slot].inode    = ino;
    ext4_handles[slot].position = 0;
    ext4_handles[slot].size     = file_size;
    ext4_handles[slot].in_use   = true;

    EXT4_VFS_END();
    return (void *)(uintptr_t)(slot + 1);
}

static int ext4_vfs_close(vfs_filesystem_t *fs, void *handle) {
    (void)fs;
    int slot = (int)(uintptr_t)handle - 1;
    if (slot < 0 || slot >= EXT4_VFS_MAX_HANDLES) return -1;
    EXT4_VFS_BEGIN();
    if (!ext4_handles[slot].in_use) { EXT4_VFS_END(); return -1; }

    ext4_handles[slot].in_use = false;
    EXT4_VFS_END();
    return 0;
}

static int ext4_vfs_read(vfs_filesystem_t *fs, void *handle,
                         void *buffer, uint32_t size) {
    (void)fs;
    int slot = (int)(uintptr_t)handle - 1;
    if (slot < 0 || slot >= EXT4_VFS_MAX_HANDLES) return -1;
    EXT4_VFS_BEGIN();
    ext4_vfs_handle_t *h = &ext4_handles[slot];
    if (!h->in_use) { EXT4_VFS_END(); return -1; }

    if (h->position >= h->size) { EXT4_VFS_END(); return 0; }

    uint32_t avail = h->size - h->position;
    if (size > avail) size = avail;

    int bytes = ext4_read_file(h->inode, buffer, h->position, size);
    if (bytes > 0) h->position += bytes;
    EXT4_VFS_END();
    return bytes;
}

static int ext4_vfs_write(vfs_filesystem_t *fs, void *handle,
                          const void *buffer, uint32_t size) {
    (void)fs;
    int slot = (int)(uintptr_t)handle - 1;
    if (slot < 0 || slot >= EXT4_VFS_MAX_HANDLES) return -1;
    EXT4_VFS_BEGIN();
    ext4_vfs_handle_t *h = &ext4_handles[slot];
    if (!h->in_use) { EXT4_VFS_END(); return -1; }

    int bytes = ext4_write_file(h->inode, buffer, h->position, size);
    if (bytes > 0) {
        h->position += bytes;
        if (h->position > h->size) h->size = h->position;
    }
    EXT4_VFS_END();
    return bytes;
}

static int ext4_vfs_seek(vfs_filesystem_t *fs, void *handle,
                         int32_t offset, int whence) {
    (void)fs;
    int slot = (int)(uintptr_t)handle - 1;
    if (slot < 0 || slot >= EXT4_VFS_MAX_HANDLES) return -1;
    EXT4_VFS_BEGIN();
    ext4_vfs_handle_t *h = &ext4_handles[slot];
    if (!h->in_use) { EXT4_VFS_END(); return -1; }

    int64_t new_pos;
    switch (whence) {
        case VFS_SEEK_SET: new_pos = offset; break;
        case VFS_SEEK_CUR: new_pos = (int64_t)h->position + offset; break;
        case VFS_SEEK_END: new_pos = (int64_t)h->size + offset; break;
        default: EXT4_VFS_END(); return -1;
    }

    if (new_pos < 0) new_pos = 0;
    h->position = (uint32_t)new_pos;
    int pos = (int)h->position;
    EXT4_VFS_END();
    return pos;
}

static int ext4_vfs_readdir(vfs_filesystem_t *fs, const char *rel_path,
                            vfs_dirent_t *entries, uint32_t max) {
    (void)fs;
    char full[VFS_MAX_PATH];
    ext4_build_full_path(full, rel_path);

    ext4_dir_list_t *ext_entries = (ext4_dir_list_t *)kalloc(
        max * sizeof(ext4_dir_list_t));
    if (!ext_entries) return -1;

    EXT4_VFS_BEGIN();
    int count = ext4_list_directory(full, ext_entries, max);
    if (count < 0) {
        EXT4_VFS_END();
        kfree(ext_entries);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        size_t nlen = strlen(ext_entries[i].name);
        if (nlen >= VFS_MAX_NAME) nlen = VFS_MAX_NAME - 1;
        memmove(entries[i].name, ext_entries[i].name, nlen);
        entries[i].name[nlen] = '\0';
        entries[i].inode = ext_entries[i].inode;
        entries[i].type  = ext_entries[i].is_directory ? VFS_DIRECTORY : VFS_FILE;
        entries[i].size  = ext_entries[i].size;
    }

    EXT4_VFS_END();
    kfree(ext_entries);
    return count;
}

static int ext4_vfs_create(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    ext4_build_full_path(full, rel_path);
    EXT4_VFS_BEGIN();
    int rc = ext4_create_file(full);
    EXT4_VFS_END();
    return rc;
}

static int ext4_vfs_mkdir(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    ext4_build_full_path(full, rel_path);
    EXT4_VFS_BEGIN();
    int rc = ext4_create_directory(full);
    EXT4_VFS_END();
    return rc;
}

static int ext4_vfs_unlink(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    ext4_build_full_path(full, rel_path);

    EXT4_VFS_BEGIN();
    uint32_t ino = ext4_lookup_path(full);
    if (ino == 0) { EXT4_VFS_END(); return -1; }

    ext2_inode_t inode;
    if (ext4_read_inode(ino, &inode) != 0) { EXT4_VFS_END(); return -1; }

    int rc = (inode.i_mode & EXT2_S_IFDIR)
        ? ext4_remove_directory(full)
        : ext4_delete_file(full);
    EXT4_VFS_END();
    return rc;
}

static int ext4_vfs_exists(vfs_filesystem_t *fs, const char *rel_path) {
    (void)fs;
    char full[VFS_MAX_PATH];
    ext4_build_full_path(full, rel_path);
    EXT4_VFS_BEGIN();
    int rc = ext4_file_exists(full);
    EXT4_VFS_END();
    return rc;
}

// ---- Static ops table -------------------------------------------------------

static vfs_fs_ops_t ext4_vfs_ops = {
    .open    = ext4_vfs_open,
    .close   = ext4_vfs_close,
    .read    = ext4_vfs_read,
    .write   = ext4_vfs_write,
    .seek    = ext4_vfs_seek,
    .readdir = ext4_vfs_readdir,
    .create  = ext4_vfs_create,
    .mkdir   = ext4_vfs_mkdir,
    .unlink  = ext4_vfs_unlink,
    .exists  = ext4_vfs_exists,
};

// ---- Public -----------------------------------------------------------------

int vfs_ext4_register(const char *mount_path) {
    if (!ext4_is_mounted()) {
        printf("vfs_ext4: ext4 not mounted – cannot register with VFS\n");
        return -1;
    }
    memset(ext4_handles, 0, sizeof(ext4_handles));
    return vfs_mount(mount_path, &ext4_vfs_ops, NULL);
}

vfs_fs_ops_t *vfs_ext4_get_ops(void) {
    return &ext4_vfs_ops;
}
