/**
 * Minimal tmpfs for /tmp — enough for Links HOME=.links and ash scratch.
 * Live install CD (Case A) mounts / as FAT32 RO, so /tmp on the ISO is not
 * writable; overlay a small in-RAM filesystem at /tmp instead.
 */
#include "tmpfs.h"
#include "vfs.h"
#include "mem.h"
#include "string.h"
#include "errno.h"
#include "utils.h"
#include "storage.h"
#include "fat32.h"
extern int printf(const char *fmt, ...);

#define TMPFS_MAX_NODES   64
#define TMPFS_MAX_NAME    64
#define TMPFS_MAX_FILE    (64 * 1024)

typedef struct tmpfs_node {
    int used;
    int is_dir;
    int parent;
    char name[TMPFS_MAX_NAME];
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
} tmpfs_node_t;

typedef struct tmpfs_handle {
    int node;
    uint32_t pos;
    uint32_t flags;
} tmpfs_handle_t;

static tmpfs_node_t nodes[TMPFS_MAX_NODES];
static int tmpfs_ready;

static void tmpfs_init_root(void) {
    memset(nodes, 0, sizeof(nodes));
    nodes[0].used = 1;
    nodes[0].is_dir = 1;
    nodes[0].parent = -1;
    nodes[0].name[0] = '\0';
    tmpfs_ready = 1;
}

static int tmpfs_find_child(int parent, const char *name) {
    for (int i = 1; i < TMPFS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == parent &&
            kstrcmp(nodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int tmpfs_alloc_node(void) {
    for (int i = 1; i < TMPFS_MAX_NODES; i++)
        if (!nodes[i].used) return i;
    return -1;
}

/* Walk rel_path from root. create_dirs/create_file make missing components. */
static int tmpfs_lookup(const char *rel_path, int create_dirs, int create_file,
                        int want_dir) {
    if (!tmpfs_ready) tmpfs_init_root();
    if (!rel_path) rel_path = "";
    while (*rel_path == '/') rel_path++;
    int cur = 0;
    if (!rel_path[0])
        return want_dir ? cur : -ENOENT;

    char part[TMPFS_MAX_NAME];
    const char *p = rel_path;
    for (;;) {
        const char *slash = p;
        while (*slash && *slash != '/') slash++;
        size_t n = (size_t)(slash - p);
        if (n == 0 || n >= TMPFS_MAX_NAME) return -EINVAL;
        memcpy(part, p, n);
        part[n] = '\0';
        int child = tmpfs_find_child(cur, part);
        int is_last = (*slash == '\0');
        if (child < 0) {
            if (is_last && create_file && !want_dir) {
                int free_i = tmpfs_alloc_node();
                if (free_i < 0) return -ENOSPC;
                memset(&nodes[free_i], 0, sizeof(nodes[free_i]));
                nodes[free_i].used = 1;
                nodes[free_i].is_dir = 0;
                nodes[free_i].parent = cur;
                kstrncpy(nodes[free_i].name, part, TMPFS_MAX_NAME);
                return free_i;
            }
            if ((!is_last && create_dirs) || (is_last && create_dirs && want_dir)) {
                int free_i = tmpfs_alloc_node();
                if (free_i < 0) return -ENOSPC;
                memset(&nodes[free_i], 0, sizeof(nodes[free_i]));
                nodes[free_i].used = 1;
                nodes[free_i].is_dir = 1;
                nodes[free_i].parent = cur;
                kstrncpy(nodes[free_i].name, part, TMPFS_MAX_NAME);
                if (is_last) return free_i;
                child = free_i;
            } else {
                return -ENOENT;
            }
        }
        if (is_last) {
            if (want_dir && !nodes[child].is_dir) return -ENOTDIR;
            if (!want_dir && nodes[child].is_dir && create_file) return -EISDIR;
            return child;
        }
        if (!nodes[child].is_dir) return -ENOTDIR;
        cur = child;
        p = slash + 1;
        while (*p == '/') p++;
        if (!*p)
            return want_dir ? cur : -ENOENT;
    }
}

static void *tmpfs_open(struct vfs_filesystem *fs, const char *rel_path, uint32_t flags) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 0, 0, 0);
    if (idx < 0 && (flags & VFS_O_CREATE))
        idx = tmpfs_lookup(rel_path, 1, 1, 0);
    if (idx < 0) return NULL;
    if (nodes[idx].is_dir) return NULL;
    if (flags & VFS_O_TRUNC)
        nodes[idx].size = 0;
    tmpfs_handle_t *h = (tmpfs_handle_t *)kalloc(sizeof(*h));
    if (!h) return NULL;
    h->node = idx;
    h->pos = (flags & VFS_O_APPEND) ? nodes[idx].size : 0;
    h->flags = flags;
    return h;
}

static int tmpfs_close(struct vfs_filesystem *fs, void *handle) {
    (void)fs;
    if (handle) kfree(handle);
    return 0;
}

static int tmpfs_read(struct vfs_filesystem *fs, void *handle, void *buffer, uint32_t size) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->node < 0 || !nodes[h->node].used) return -EBADF;
    tmpfs_node_t *n = &nodes[h->node];
    if (h->pos >= n->size) return 0;
    uint32_t avail = n->size - h->pos;
    if (size > avail) size = avail;
    if (size && n->data)
        memcpy(buffer, n->data + h->pos, size);
    h->pos += size;
    return (int)size;
}

static int tmpfs_grow(tmpfs_node_t *n, uint32_t need) {
    if (need > TMPFS_MAX_FILE) return -ENOSPC;
    if (n->cap >= need) return 0;
    uint32_t ncap = n->cap ? n->cap : 512;
    while (ncap < need) {
        if (ncap > TMPFS_MAX_FILE / 2) { ncap = TMPFS_MAX_FILE; break; }
        ncap *= 2;
    }
    if (ncap > TMPFS_MAX_FILE) ncap = TMPFS_MAX_FILE;
    uint8_t *nd = (uint8_t *)kalloc(ncap);
    if (!nd) return -ENOMEM;
    if (n->data && n->size)
        memcpy(nd, n->data, n->size);
    if (n->data) kfree(n->data);
    n->data = nd;
    n->cap = ncap;
    return 0;
}

static int tmpfs_write(struct vfs_filesystem *fs, void *handle, const void *buffer, uint32_t size) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->node < 0 || !nodes[h->node].used) return -EBADF;
    if (!(h->flags & VFS_O_WRITE)) return -EBADF;
    tmpfs_node_t *n = &nodes[h->node];
    uint32_t end = h->pos + size;
    int rc = tmpfs_grow(n, end);
    if (rc < 0) return rc;
    memcpy(n->data + h->pos, buffer, size);
    h->pos += size;
    if (h->pos > n->size) n->size = h->pos;
    return (int)size;
}

static int tmpfs_seek(struct vfs_filesystem *fs, void *handle, int32_t offset, int whence) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->node < 0) return -EBADF;
    tmpfs_node_t *n = &nodes[h->node];
    int64_t np;
    if (whence == VFS_SEEK_SET) np = offset;
    else if (whence == VFS_SEEK_CUR) np = (int64_t)h->pos + offset;
    else if (whence == VFS_SEEK_END) np = (int64_t)n->size + offset;
    else return -EINVAL;
    if (np < 0) return -EINVAL;
    h->pos = (uint32_t)np;
    return (int)h->pos;
}

static int tmpfs_get_size(struct vfs_filesystem *fs, void *handle) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->node < 0) return -EBADF;
    return (int)nodes[h->node].size;
}

static int tmpfs_readdir(struct vfs_filesystem *fs, const char *rel_path,
                         vfs_dirent_t *entries, uint32_t max_entries) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path ? rel_path : "", 0, 0, 1);
    if (idx < 0) return idx;
    uint32_t count = 0;
    for (int i = 1; i < TMPFS_MAX_NODES && count < max_entries; i++) {
        if (!nodes[i].used || nodes[i].parent != idx) continue;
        kstrncpy(entries[count].name, nodes[i].name, VFS_MAX_NAME);
        entries[count].inode = (uint32_t)i;
        entries[count].type = nodes[i].is_dir ? VFS_DIRECTORY : VFS_FILE;
        entries[count].size = nodes[i].size;
        count++;
    }
    return (int)count;
}

static int tmpfs_create(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 1, 1, 0);
    return idx < 0 ? idx : 0;
}

static int tmpfs_mkdir(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 1, 0, 1);
    return idx < 0 ? idx : 0;
}

static int tmpfs_unlink(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 0, 0, 0);
    if (idx < 0) {
        idx = tmpfs_lookup(rel_path, 0, 0, 1);
        if (idx < 0) return idx;
        for (int i = 1; i < TMPFS_MAX_NODES; i++)
            if (nodes[i].used && nodes[i].parent == idx)
                return -EBUSY;
    }
    if (idx == 0) return -EBUSY;
    if (nodes[idx].data) kfree(nodes[idx].data);
    memset(&nodes[idx], 0, sizeof(nodes[idx]));
    return 0;
}

static int tmpfs_exists(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    if (!rel_path || !rel_path[0]) return 1;
    if (tmpfs_lookup(rel_path, 0, 0, 0) >= 0) return 1;
    return tmpfs_lookup(rel_path, 0, 0, 1) >= 0 ? 1 : 0;
}

static int tmpfs_stat(struct vfs_filesystem *fs, const char *rel_path, struct vfs_stat *st) {
    (void)fs;
    if (!st) return -EINVAL;
    int idx;
    if (!rel_path || !rel_path[0]) idx = 0;
    else {
        idx = tmpfs_lookup(rel_path, 0, 0, 0);
        if (idx < 0) idx = tmpfs_lookup(rel_path, 0, 0, 1);
        if (idx < 0) return idx;
    }
    memset(st, 0, sizeof(*st));
    st->ino = (uint64_t)idx;
    st->nlink = 1;
    st->blksize = 512;
    if (nodes[idx].is_dir) {
        st->mode = 0040755;
        st->size = 0;
    } else {
        st->mode = 0100644;
        st->size = nodes[idx].size;
        st->blocks = (nodes[idx].size + 511) / 512;
    }
    return 0;
}

static int tmpfs_truncate(struct vfs_filesystem *fs, const char *rel_path, uint64_t len) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 0, 0, 0);
    if (idx < 0) return idx;
    if (len > TMPFS_MAX_FILE) return -ENOSPC;
    if (len > nodes[idx].size) {
        int rc = tmpfs_grow(&nodes[idx], (uint32_t)len);
        if (rc < 0) return rc;
        memset(nodes[idx].data + nodes[idx].size, 0, (uint32_t)len - nodes[idx].size);
    }
    nodes[idx].size = (uint32_t)len;
    return 0;
}

static int tmpfs_ftruncate(struct vfs_filesystem *fs, void *handle, uint64_t len) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->node < 0) return -EBADF;
    if (len > TMPFS_MAX_FILE) return -ENOSPC;
    tmpfs_node_t *n = &nodes[h->node];
    if (len > n->size) {
        int rc = tmpfs_grow(n, (uint32_t)len);
        if (rc < 0) return rc;
        memset(n->data + n->size, 0, (uint32_t)len - n->size);
    }
    n->size = (uint32_t)len;
    if (h->pos > n->size) h->pos = n->size;
    return 0;
}

static vfs_fs_ops_t tmpfs_ops = {
    .open = tmpfs_open,
    .close = tmpfs_close,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .seek = tmpfs_seek,
    .get_size = tmpfs_get_size,
    .readdir = tmpfs_readdir,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink,
    .exists = tmpfs_exists,
    .stat = tmpfs_stat,
    .truncate = tmpfs_truncate,
    .ftruncate = tmpfs_ftruncate,
};

int tmpfs_mount_tmp(void) {
    /* Only overlay /tmp when / is the read-only install CD (Case A). On a
     * writable disk root the real /tmp must stay visible: ostest and other
     * tools need rename/rmdir/large files there, which this ramfs lacks. */
    if (!fat32_filesystem.is_mounted || !fat32_filesystem.storage_dev ||
        !fat32_filesystem.storage_dev->optical)
        return 0;
    if (!tmpfs_ready) tmpfs_init_root();
    /* vfs_find_mount("/tmp") matches root "/" — do not treat that as already
     * mounted. vfs_mount checks exact path duplicates and returns -EBUSY. */
    int r = vfs_mount("/tmp", &tmpfs_ops, NULL);
    if (r == 0)
        printf("tmpfs: mounted writable /tmp (ram)\n");
    else if (r == -EBUSY)
        return 0;
    return r;
}
