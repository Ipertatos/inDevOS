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
#ifndef ENOTEMPTY
#define ENOTEMPTY 39      /* Linux value */
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

#define TMPFS_MAX_NODES   1024
#define TMPFS_MAX_NAME    64
/* File data lives in fixed 64 KiB chunks (no large contiguous kalloc, no
 * realloc+copy).  Old code capped every file at 64 KiB and the fs at 64
 * nodes: `cp sdltest(1.45 MB) /tmp` failed with ENOSPC. */
#define TMPFS_CHUNK       (64u * 1024u)
#define TMPFS_MIN_BYTES   (64ull * 1024 * 1024)
#define TMPFS_RESERVE     (48ull * 1024 * 1024)   /* never eat the last 48 MB of RAM */

typedef struct tmpfs_node {
    int used;
    int is_dir;
    int parent;
    char name[TMPFS_MAX_NAME];
    uint8_t **chunks;      /* nchunk_slots pointers, NULL = hole (reads as 0) */
    uint32_t nchunk_slots;
    uint32_t size;
} tmpfs_node_t;

typedef struct tmpfs_handle {
    int node;
    uint32_t pos;
    uint32_t flags;
} tmpfs_handle_t;

static tmpfs_node_t nodes[TMPFS_MAX_NODES];
static int tmpfs_ready;
static uint64_t tmpfs_limit_bytes;   /* size cap, set at mount from RAM */
static uint64_t tmpfs_used_bytes;    /* chunk bytes allocated */

static void tmpfs_init_root(void) {
    memset(nodes, 0, sizeof(nodes));
    nodes[0].used = 1;
    nodes[0].is_dir = 1;
    nodes[0].parent = -1;
    nodes[0].name[0] = '\0';
    tmpfs_ready = 1;
}

/* 25% of free RAM at mount time, at least 64 MB, but never past what is
 * actually free minus a reserve (checked again on every chunk). */
static void tmpfs_size_from_ram(void) {
    uint64_t free_b = num_pages_available * (uint64_t)PAGE_SIZE;
    uint64_t lim = free_b / 4;
    if (lim < TMPFS_MIN_BYTES) lim = TMPFS_MIN_BYTES;
    tmpfs_limit_bytes = lim;
}

uint64_t tmpfs_limit(void) { return tmpfs_limit_bytes; }
uint64_t tmpfs_used(void) { return tmpfs_used_bytes; }

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

static void tmpfs_free_data(tmpfs_node_t *n) {
    if (n->chunks) {
        for (uint32_t i = 0; i < n->nchunk_slots; i++)
            if (n->chunks[i]) { kfree(n->chunks[i]); tmpfs_used_bytes -= TMPFS_CHUNK; }
        kfree(n->chunks);
    }
    n->chunks = NULL;
    n->nchunk_slots = 0;
}

/* Drop chunks wholly past @len and zero the tail of the last one. */
static void tmpfs_shrink(tmpfs_node_t *n, uint32_t len) {
    uint32_t keep = (len + TMPFS_CHUNK - 1) / TMPFS_CHUNK;
    for (uint32_t i = keep; i < n->nchunk_slots; i++)
        if (n->chunks[i]) { kfree(n->chunks[i]); n->chunks[i] = NULL; tmpfs_used_bytes -= TMPFS_CHUNK; }
    if (len % TMPFS_CHUNK && keep && keep <= n->nchunk_slots && n->chunks[keep - 1])
        memset(n->chunks[keep - 1] + len % TMPFS_CHUNK, 0, TMPFS_CHUNK - len % TMPFS_CHUNK);
}

/* Make sure chunk slot table covers @need bytes (no data allocated). */
static int tmpfs_slots(tmpfs_node_t *n, uint64_t need) {
    if (need > 0xFFFFFFFFull) return -ENOSPC;
    if (need > tmpfs_limit_bytes) return -ENOSPC;
    uint32_t want = (uint32_t)((need + TMPFS_CHUNK - 1) / TMPFS_CHUNK);
    if (want <= n->nchunk_slots) return 0;
    uint32_t ns = n->nchunk_slots ? n->nchunk_slots : 4;
    while (ns < want) ns *= 2;
    uint8_t **t = (uint8_t **)kalloc(ns * sizeof(uint8_t *));
    if (!t) return -ENOSPC;
    memset(t, 0, ns * sizeof(uint8_t *));
    if (n->chunks) {
        memcpy(t, n->chunks, n->nchunk_slots * sizeof(uint8_t *));
        kfree(n->chunks);
    }
    n->chunks = t;
    n->nchunk_slots = ns;
    return 0;
}

static uint8_t *tmpfs_chunk(tmpfs_node_t *n, uint32_t ci) {
    if (ci >= n->nchunk_slots) return NULL;
    if (n->chunks[ci]) return n->chunks[ci];
    if (tmpfs_used_bytes + TMPFS_CHUNK > tmpfs_limit_bytes) return NULL;
    if (num_pages_available * (uint64_t)PAGE_SIZE < TMPFS_RESERVE + TMPFS_CHUNK) return NULL;
    uint8_t *c = (uint8_t *)kalloc(TMPFS_CHUNK);
    if (!c) return NULL;
    memset(c, 0, TMPFS_CHUNK);
    n->chunks[ci] = c;
    tmpfs_used_bytes += TMPFS_CHUNK;
    return c;
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
        if (n == 0 || n >= TMPFS_MAX_NAME) return n ? -ENAMETOOLONG : -EINVAL;
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
    if (flags & VFS_O_TRUNC) {
        tmpfs_free_data(&nodes[idx]);
        nodes[idx].size = 0;
    }
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
    uint32_t done = 0;
    while (done < size) {
        uint32_t off = h->pos + done;
        uint32_t ci = off / TMPFS_CHUNK, co = off % TMPFS_CHUNK;
        uint32_t c = TMPFS_CHUNK - co;
        if (c > size - done) c = size - done;
        uint8_t *src = (ci < n->nchunk_slots) ? n->chunks[ci] : NULL;
        if (src) memcpy((uint8_t *)buffer + done, src + co, c);
        else memset((uint8_t *)buffer + done, 0, c);
        done += c;
    }
    h->pos += size;
    return (int)size;
}

static int tmpfs_write(struct vfs_filesystem *fs, void *handle, const void *buffer, uint32_t size) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->node < 0 || !nodes[h->node].used) return -EBADF;
    if (!(h->flags & VFS_O_WRITE)) return -EBADF;
    tmpfs_node_t *n = &nodes[h->node];
    if (h->flags & VFS_O_APPEND) h->pos = n->size;
    if (!size) return 0;
    int rc = tmpfs_slots(n, (uint64_t)h->pos + size);
    if (rc < 0) return rc;
    uint32_t done = 0;
    while (done < size) {
        uint32_t off = h->pos + done;
        uint32_t ci = off / TMPFS_CHUNK, co = off % TMPFS_CHUNK;
        uint32_t c = TMPFS_CHUNK - co;
        if (c > size - done) c = size - done;
        uint8_t *dst = tmpfs_chunk(n, ci);
        if (!dst) break;
        memcpy(dst + co, (const uint8_t *)buffer + done, c);
        done += c;
    }
    if (!done) return -ENOSPC;
    h->pos += done;
    if (h->pos > n->size) n->size = h->pos;
    return (int)done;
}

/* Resize to @len: new bytes read as zero (holes), shrinking frees chunks. */
static int tmpfs_set_len(tmpfs_node_t *n, uint64_t len) {
    if (len > n->size) {
        int rc = tmpfs_slots(n, len);
        if (rc < 0) return rc;
    } else {
        tmpfs_shrink(n, (uint32_t)len);
    }
    n->size = (uint32_t)len;
    return 0;
}

static int tmpfs_rename(struct vfs_filesystem *fs, const char *old_rel, const char *new_rel) {
    (void)fs;
    int src = tmpfs_lookup(old_rel, 0, 0, 0);
    if (src < 0) src = tmpfs_lookup(old_rel, 0, 0, 1);
    if (src <= 0) return src < 0 ? src : -EBUSY;
    /* split new path into parent dir + last name */
    char tmp[256];
    kstrncpy(tmp, new_rel ? new_rel : "", sizeof(tmp));
    char *t = tmp;
    while (*t == '/') t++;
    size_t L = strlen(t);
    while (L && t[L - 1] == '/') t[--L] = 0;
    if (!L) return -EINVAL;
    char *slash = NULL;
    for (char *q = t; *q; q++) if (*q == '/') slash = q;
    const char *leaf = slash ? slash + 1 : t;
    int dparent;
    if (slash) { *slash = 0; dparent = tmpfs_lookup(t, 0, 0, 1); }
    else dparent = 0;
    if (dparent < 0) return dparent;
    if (strlen(leaf) >= TMPFS_MAX_NAME) return -ENAMETOOLONG;
    for (int a = dparent; a >= 0; a = nodes[a].parent)   /* no dir into itself */
        if (a == src) return -EINVAL;
    int dst = tmpfs_find_child(dparent, leaf);
    if (dst == src) return 0;
    if (dst > 0) {
        if (nodes[dst].is_dir != nodes[src].is_dir)
            return nodes[dst].is_dir ? -EISDIR : -ENOTDIR;
        if (nodes[dst].is_dir)
            for (int i = 1; i < TMPFS_MAX_NODES; i++)
                if (nodes[i].used && nodes[i].parent == dst) return -ENOTEMPTY;
        tmpfs_free_data(&nodes[dst]);
        memset(&nodes[dst], 0, sizeof(nodes[dst]));
    }
    nodes[src].parent = dparent;
    kstrncpy(nodes[src].name, leaf, TMPFS_MAX_NAME);
    return 0;
}

static int tmpfs_rmdir(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 0, 0, 1);
    if (idx < 0) return idx;
    if (idx == 0) return -EBUSY;
    for (int i = 1; i < TMPFS_MAX_NODES; i++)
        if (nodes[i].used && nodes[i].parent == idx) return -ENOTEMPTY;
    memset(&nodes[idx], 0, sizeof(nodes[idx]));
    return 0;
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
    tmpfs_free_data(&nodes[idx]);
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
    if (nodes[idx].is_dir) return -EISDIR;
    return tmpfs_set_len(&nodes[idx], len);
}

static int tmpfs_ftruncate(struct vfs_filesystem *fs, void *handle, uint64_t len) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->node < 0) return -EBADF;
    return tmpfs_set_len(&nodes[h->node], len);
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
    .rename = tmpfs_rename,
    .rmdir = tmpfs_rmdir,
};

int tmpfs_mount_tmp(void) {
    /* Only overlay /tmp when / is the read-only install CD (Case A). On a
     * writable disk root the real /tmp must stay visible: ostest and other
     * tools need rename/rmdir/large files there, which this ramfs lacks. */
    if (!fat32_filesystem.is_mounted || !fat32_filesystem.storage_dev ||
        !fat32_filesystem.storage_dev->optical)
        return 0;
    if (!tmpfs_ready) tmpfs_init_root();
    tmpfs_size_from_ram();
    /* vfs_find_mount("/tmp") matches root "/" — do not treat that as already
     * mounted. vfs_mount checks exact path duplicates and returns -EBUSY. */
    int r = vfs_mount("/tmp", &tmpfs_ops, NULL);
    if (r == 0)
        printf("tmpfs: mounted writable /tmp (ram, limit %lu MB, %lu MB free)\n",
               (unsigned long)(tmpfs_limit_bytes >> 20),
               (unsigned long)((num_pages_available * (uint64_t)PAGE_SIZE) >> 20));
    else if (r == -EBUSY)
        return 0;
    return r;
}
