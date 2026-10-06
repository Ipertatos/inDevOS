/**
 * Minimal /dev/dri VFS backend — open card0 / renderD128 and ioctl -> drm_ioctl().
 */
#include "vfs.h"
#include "drm.h"
#include "mem.h"
#include "string.h"

struct dri_handle {
    int drm_minor;
};

static void *dev_dri_open(struct vfs_filesystem *fs, const char *rel_path, uint32_t flags) {
    (void)fs;
    (void)flags;
    if (!rel_path || !rel_path[0])
        return NULL;

    struct dri_handle *h = (struct dri_handle *)kalloc(sizeof(struct dri_handle));
    if (!h)
        return NULL;

    if (kstrcmp(rel_path, "card0") == 0)
        h->drm_minor = DRM_MINOR_PRIMARY;
    else if (kstrcmp(rel_path, "renderD128") == 0)
        h->drm_minor = DRM_MINOR_RENDER;
    else {
        kfree(h);
        return NULL;
    }
    return h;
}

static int dev_dri_close(struct vfs_filesystem *fs, void *handle) {
    (void)fs;
    if (handle)
        kfree(handle);
    return 0;
}

static int dev_dri_read(struct vfs_filesystem *fs, void *handle, void *buffer, uint32_t size) {
    (void)fs;
    (void)handle;
    (void)buffer;
    (void)size;
    return -1;
}

static int dev_dri_write(struct vfs_filesystem *fs, void *handle, const void *buffer, uint32_t size) {
    (void)fs;
    (void)handle;
    (void)buffer;
    (void)size;
    return -1;
}

static int dev_dri_seek(struct vfs_filesystem *fs, void *handle, int32_t offset, int whence) {
    (void)fs;
    (void)handle;
    (void)offset;
    (void)whence;
    return -1;
}

static int dev_dri_readdir(struct vfs_filesystem *fs, const char *rel_path,
                           vfs_dirent_t *entries, uint32_t max_entries) {
    (void)fs;
    (void)rel_path;
    (void)entries;
    (void)max_entries;
    return -1;
}

static int dev_dri_create(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    (void)rel_path;
    return -1;
}

static int dev_dri_mkdir(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    (void)rel_path;
    return -1;
}

static int dev_dri_unlink(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    (void)rel_path;
    return -1;
}

static int dev_dri_exists(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    if (!rel_path)
        return 0;
    if (kstrcmp(rel_path, "card0") == 0 || kstrcmp(rel_path, "renderD128") == 0)
        return 1;
    return 0;
}

static int dev_dri_ioctl(struct vfs_filesystem *fs, void *handle, unsigned long cmd, void *arg) {
    (void)fs;
    struct dri_handle *h = (struct dri_handle *)handle;
    if (!h)
        return -22;
    return drm_ioctl(h->drm_minor, (uint32_t)cmd, arg);
}

static vfs_fs_ops_t dev_dri_vfs_ops = {
    .open    = dev_dri_open,
    .close   = dev_dri_close,
    .read    = dev_dri_read,
    .write   = dev_dri_write,
    .seek    = dev_dri_seek,
    .readdir = dev_dri_readdir,
    .create  = dev_dri_create,
    .mkdir   = dev_dri_mkdir,
    .unlink  = dev_dri_unlink,
    .exists  = dev_dri_exists,
    .ioctl   = dev_dri_ioctl,
};

int dev_dri_vfs_mount(void) {
    return vfs_mount("/dev/dri", &dev_dri_vfs_ops, NULL);
}
