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

/* Positions past end of file.  fat32_seek() clamps to the file size, but
 * POSIX lets lseek() go past EOF and the next write() then fills the gap
 * with zeros.  GNU as/ld write their output that way (section data first,
 * at its final offset, the ELF header last), so the clamp made them append
 * every chunk at EOF and produce corrupt objects.  The adapter keeps the
 * requested position per handle and zero-fills the gap on the next write;
 * a read there returns 0 (EOF) like Linux. */
static uint32_t fat32_vpos[FAT32_MAX_OPEN_FILES];
static bool     fat32_vpend[FAT32_MAX_OPEN_FILES];

static int fat32_hidx(fat32_file_t *f) {
    if (!f || f < &open_files[0] || f >= &open_files[FAT32_MAX_OPEN_FILES]) return -1;
    return (int)(f - &open_files[0]);
}

/* Zero-fill from EOF up to the pending position; leaves the file position
 * at that offset.  0 or -1. */
static int fat32_fill_gap(fat32_file_t *f, int i) {
    static const uint8_t zeros[512];
    uint32_t want = fat32_vpos[i];
    fat32_vpend[i] = false;
    if (want <= f->file_size) return fat32_seek(f, want) < 0 ? -1 : 0;
    if (fat32_seek(f, f->file_size) < 0) return -1;
    while (f->file_size < want) {
        uint32_t n = want - f->file_size;
        if (n > sizeof(zeros)) n = sizeof(zeros);
        int w = fat32_write(f, zeros, n);
        if (w <= 0) return -1;
    }
    return 0;
}

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
    int i = fat32_hidx(f);
    if (i >= 0) fat32_vpend[i] = false;
    return (void *)f;
}

static int fat32_vfs_close(vfs_filesystem_t *fs, void *handle) {
    (void)fs;
    int i = fat32_hidx((fat32_file_t *)handle);
    if (i >= 0) fat32_vpend[i] = false;
    return fat32_close((fat32_file_t *)handle);
}

static int fat32_vfs_read(vfs_filesystem_t *fs, void *handle,
                          void *buffer, uint32_t size) {
    (void)fs;
    fat32_file_t *f = (fat32_file_t *)handle;
    int i = fat32_hidx(f);
    if (i >= 0 && fat32_vpend[i]) {
        if (fat32_vpos[i] >= f->file_size) return 0;      /* still past EOF */
        if (fat32_fill_gap(f, i) < 0) return -1;          /* file grew meanwhile */
    }
    return fat32_read(f, buffer, size);
}

static int fat32_vfs_write(vfs_filesystem_t *fs, void *handle,
                           const void *buffer, uint32_t size) {
    (void)fs;
    fat32_file_t *f = (fat32_file_t *)handle;
    int i = fat32_hidx(f);
    if (i >= 0 && fat32_vpend[i] && fat32_fill_gap(f, i) < 0) return -1;
    return fat32_write(f, buffer, size);
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

    int i = fat32_hidx(f);
    int64_t cur = (i >= 0 && fat32_vpend[i]) ? (int64_t)fat32_vpos[i] : (int64_t)f->file_position;
    int64_t np;
    switch (whence) {
        case VFS_SEEK_SET: np = offset; break;
        case VFS_SEEK_CUR: np = cur + offset; break;
        case VFS_SEEK_END: np = (int64_t)f->file_size + offset; break;
        default: return -1;
    }
    if (np < 0 || np > 0x7fffffffLL) return -1;
    if (i >= 0) fat32_vpend[i] = false;
    if ((uint64_t)np > f->file_size && i >= 0) {
        /* Past EOF: park at EOF, remember the real position. */
        if (fat32_seek(f, f->file_size) < 0) return -1;
        fat32_vpos[i] = (uint32_t)np;
        fat32_vpend[i] = true;
        return (int)np;
    }
    if (fat32_seek(f, (uint32_t)np) < 0) return -1;
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
