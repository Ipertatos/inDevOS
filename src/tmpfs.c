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
#include "rtc.h"
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
#define TMPFS_LINK_MAX    1023                    /* symlink target length */

/* Inodes hold the data and attributes; directory entries (nodes) name them.
 * Hard links are several nodes sharing one inode; an unlinked inode stays
 * alive while open handles reference it (read/write through the fd works). */
typedef struct tmpfs_inode {
    int used;
    uint32_t mode;         /* S_IFREG/S_IFDIR/S_IFLNK | permission bits */
    uint32_t uid, gid;
    uint32_t nlink;        /* directory entries naming it (files, links) */
    int opens;             /* open handles */
    uint8_t **chunks;      /* nchunk_slots pointers, NULL = hole (reads as 0) */
    uint32_t nchunk_slots;
    uint32_t size;
    char *target;          /* symlink target (kalloc'd) */
    int64_t mtime, ctime;
} tmpfs_inode_t;

typedef struct tmpfs_node {
    int used;
    int parent;
    int ino;
    char name[TMPFS_MAX_NAME];
} tmpfs_node_t;

typedef struct tmpfs_handle {
    int ino;
    uint32_t pos;
    uint32_t flags;
} tmpfs_handle_t;

#define T_IFMT  0170000u
#define T_IFDIR 0040000u
#define T_IFREG 0100000u
#define T_IFLNK 0120000u

static tmpfs_node_t nodes[TMPFS_MAX_NODES];
static tmpfs_inode_t inodes[TMPFS_MAX_NODES];
static int tmpfs_ready;
static uint64_t tmpfs_limit_bytes;   /* size cap, set at mount from RAM */
static uint64_t tmpfs_used_bytes;    /* chunk bytes allocated */

#define NI(i) (&inodes[nodes[(i)].ino])
static int tmpfs_isdir(int i) { return (NI(i)->mode & T_IFMT) == T_IFDIR; }
static int tmpfs_islnk(int i) { return (NI(i)->mode & T_IFMT) == T_IFLNK; }
static int64_t tmpfs_now(void) { return (int64_t)rtc_unix_epoch(); }

static void tmpfs_init_root(void) {
    memset(nodes, 0, sizeof(nodes));
    memset(inodes, 0, sizeof(inodes));
    nodes[0].used = 1;
    nodes[0].parent = -1;
    nodes[0].ino = 0;
    nodes[0].name[0] = '\0';
    inodes[0].used = 1;
    inodes[0].mode = T_IFDIR | 01777;                  /* /tmp: sticky, world-writable */
    inodes[0].nlink = 1;
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
static int tmpfs_has_children(int dir) {
    for (int i = 1; i < TMPFS_MAX_NODES; i++)
        if (nodes[i].used && nodes[i].parent == dir) return 1;
    return 0;
}

static int tmpfs_alloc_node(void) {
    for (int i = 1; i < TMPFS_MAX_NODES; i++)
        if (!nodes[i].used) return i;
    return -1;
}
static int tmpfs_alloc_inode(uint32_t mode) {
    for (int i = 1; i < TMPFS_MAX_NODES; i++)
        if (!inodes[i].used) {
            memset(&inodes[i], 0, sizeof(inodes[i]));
            inodes[i].used = 1;
            inodes[i].mode = mode;
            inodes[i].nlink = 1;
            inodes[i].mtime = inodes[i].ctime = tmpfs_now();
            return i;
        }
    return -1;
}

static void tmpfs_free_data(tmpfs_inode_t *n) {
    if (n->chunks) {
        for (uint32_t i = 0; i < n->nchunk_slots; i++)
            if (n->chunks[i]) { kfree(n->chunks[i]); tmpfs_used_bytes -= TMPFS_CHUNK; }
        kfree(n->chunks);
    }
    n->chunks = NULL;
    n->nchunk_slots = 0;
}
static void tmpfs_put_inode(int ino) {
    tmpfs_inode_t *n = &inodes[ino];
    if (ino <= 0 || !n->used || n->nlink || n->opens) return;
    tmpfs_free_data(n);
    if (n->target) kfree(n->target);
    memset(n, 0, sizeof(*n));
}
/* new entry @name in directory @parent for inode @ino */
static int tmpfs_add_node(int parent, const char *name, int ino) {
    int i = tmpfs_alloc_node();
    if (i < 0) return -ENOSPC;
    memset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].used = 1;
    nodes[i].parent = parent;
    nodes[i].ino = ino;
    kstrncpy(nodes[i].name, name, TMPFS_MAX_NAME);
    inodes[parent >= 0 ? nodes[parent].ino : 0].mtime = tmpfs_now();
    return i;
}
/* drop directory entry @i; the inode goes when no entry or handle is left */
static void tmpfs_drop_node(int i) {
    int ino = nodes[i].ino;
    if (nodes[i].parent >= 0) inodes[nodes[nodes[i].parent].ino].mtime = tmpfs_now();
    memset(&nodes[i], 0, sizeof(nodes[i]));
    if (inodes[ino].nlink) inodes[ino].nlink--;
    inodes[ino].ctime = tmpfs_now();
    tmpfs_put_inode(ino);
}

/* Drop chunks wholly past @len and zero the tail of the last one. */
static void tmpfs_shrink(tmpfs_inode_t *n, uint32_t len) {
    uint32_t keep = (len + TMPFS_CHUNK - 1) / TMPFS_CHUNK;
    for (uint32_t i = keep; i < n->nchunk_slots; i++)
        if (n->chunks[i]) { kfree(n->chunks[i]); n->chunks[i] = NULL; tmpfs_used_bytes -= TMPFS_CHUNK; }
    if (len % TMPFS_CHUNK && keep && keep <= n->nchunk_slots && n->chunks[keep - 1])
        memset(n->chunks[keep - 1] + len % TMPFS_CHUNK, 0, TMPFS_CHUNK - len % TMPFS_CHUNK);
}

/* Make sure chunk slot table covers @need bytes (no data allocated). */
static int tmpfs_slots(tmpfs_inode_t *n, uint64_t need) {
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

static uint8_t *tmpfs_chunk(tmpfs_inode_t *n, uint32_t ci) {
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

/* Walk rel_path from root (symlinks are not followed: the VFS resolver has
 * expanded them; a link in the middle of a path is ENOTDIR here).
 * create_dirs/create_file make missing components. */
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
                int ino = tmpfs_alloc_inode(T_IFREG | 0644);
                if (ino < 0) return -ENOSPC;
                int r = tmpfs_add_node(cur, part, ino);
                if (r < 0) { inodes[ino].nlink = 0; tmpfs_put_inode(ino); }
                return r;
            }
            if ((!is_last && create_dirs) || (is_last && create_dirs && want_dir)) {
                int ino = tmpfs_alloc_inode(T_IFDIR | 0755);
                if (ino < 0) return -ENOSPC;
                int r = tmpfs_add_node(cur, part, ino);
                if (r < 0) { inodes[ino].nlink = 0; tmpfs_put_inode(ino); return r; }
                if (is_last) return r;
                child = r;
            } else {
                return -ENOENT;
            }
        }
        if (is_last) {
            if (want_dir && !tmpfs_isdir(child)) return -ENOTDIR;
            if (!want_dir && tmpfs_isdir(child) && create_file) return -EISDIR;
            return child;
        }
        if (!tmpfs_isdir(child)) return -ENOTDIR;
        cur = child;
        p = slash + 1;
        while (*p == '/') p++;
        if (!*p)
            return want_dir ? cur : -ENOENT;
    }
}
/* any entry (file, dir or link) */
static int tmpfs_lookup_any(const char *rel) {
    if (!rel || !rel[0]) return 0;
    int i = tmpfs_lookup(rel, 0, 0, 0);
    if (i < 0 && i != -ENOTDIR && i != -ENAMETOOLONG) {
        int j = tmpfs_lookup(rel, 0, 0, 1);
        if (j >= 0) return j;
    }
    return i;
}
/* parent directory of @rel and its last component in @leaf */
static int tmpfs_parent(const char *rel, char *leaf) {
    char tmp[512];
    kstrncpy(tmp, rel ? rel : "", sizeof(tmp));
    char *t = tmp;
    while (*t == '/') t++;
    size_t L = strlen(t);
    while (L && t[L - 1] == '/') t[--L] = 0;
    if (!L) return -EEXIST;                         /* the mount root */
    char *slash = NULL;
    for (char *q = t; *q; q++) if (*q == '/') slash = q;
    const char *nm = slash ? slash + 1 : t;
    if (strlen(nm) >= TMPFS_MAX_NAME) return -ENAMETOOLONG;
    kstrncpy(leaf, nm, TMPFS_MAX_NAME);
    if (!slash) { if (!tmpfs_ready) tmpfs_init_root(); return 0; }
    *slash = 0;
    return tmpfs_lookup(t, 0, 0, 1);
}

static void *tmpfs_open(struct vfs_filesystem *fs, const char *rel_path, uint32_t flags) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 0, 0, 0);
    if (idx < 0 && (flags & VFS_O_CREATE))
        idx = tmpfs_lookup(rel_path, 1, 1, 0);
    if (idx < 0) return NULL;
    if (tmpfs_isdir(idx) || tmpfs_islnk(idx)) return NULL;
    tmpfs_inode_t *in = NI(idx);
    if (flags & VFS_O_TRUNC) {
        tmpfs_free_data(in);
        in->size = 0;
        in->mtime = in->ctime = tmpfs_now();
    }
    tmpfs_handle_t *h = (tmpfs_handle_t *)kalloc(sizeof(*h));
    if (!h) return NULL;
    h->ino = nodes[idx].ino;
    h->pos = (flags & VFS_O_APPEND) ? in->size : 0;
    h->flags = flags;
    in->opens++;
    return h;
}

static tmpfs_inode_t *tmpfs_hin(void *handle) {
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    if (!h || h->ino <= 0 || h->ino >= TMPFS_MAX_NODES || !inodes[h->ino].used) return NULL;
    return &inodes[h->ino];
}

static int tmpfs_close(struct vfs_filesystem *fs, void *handle) {
    (void)fs;
    if (!handle) return 0;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    tmpfs_inode_t *in = tmpfs_hin(handle);
    if (in) {
        if (in->opens > 0) in->opens--;
        tmpfs_put_inode(h->ino);
    }
    kfree(handle);
    return 0;
}

static int tmpfs_read(struct vfs_filesystem *fs, void *handle, void *buffer, uint32_t size) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    tmpfs_inode_t *n = tmpfs_hin(handle);
    if (!n) return -EBADF;
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
    tmpfs_inode_t *n = tmpfs_hin(handle);
    if (!n) return -EBADF;
    if (!(h->flags & VFS_O_WRITE)) return -EBADF;
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
    n->mtime = n->ctime = tmpfs_now();
    return (int)done;
}

/* Resize to @len: new bytes read as zero (holes), shrinking frees chunks. */
static int tmpfs_set_len(tmpfs_inode_t *n, uint64_t len) {
    if (len > n->size) {
        int rc = tmpfs_slots(n, len);
        if (rc < 0) return rc;
    } else {
        tmpfs_shrink(n, (uint32_t)len);
    }
    n->size = (uint32_t)len;
    n->mtime = n->ctime = tmpfs_now();
    return 0;
}

static int tmpfs_rename(struct vfs_filesystem *fs, const char *old_rel, const char *new_rel) {
    (void)fs;
    int src = tmpfs_lookup_any(old_rel);
    if (src <= 0) return src < 0 ? src : -EBUSY;
    char leaf[TMPFS_MAX_NAME];
    int dparent = tmpfs_parent(new_rel, leaf);
    if (dparent == -EEXIST) return -EBUSY;
    if (dparent < 0) return dparent;
    for (int a = dparent; a >= 0; a = nodes[a].parent)   /* no dir into itself */
        if (a == src) return -EINVAL;
    int dst = tmpfs_find_child(dparent, leaf);
    if (dst == src) return 0;
    if (dst > 0) {
        if (nodes[dst].ino == nodes[src].ino) return 0;  /* hard links to one inode: no-op */
        if (tmpfs_isdir(dst) != tmpfs_isdir(src))
            return tmpfs_isdir(dst) ? -EISDIR : -ENOTDIR;
        if (tmpfs_isdir(dst) && tmpfs_has_children(dst)) return -ENOTEMPTY;
        tmpfs_drop_node(dst);
    }
    inodes[nodes[nodes[src].parent].ino].mtime = tmpfs_now();
    nodes[src].parent = dparent;
    kstrncpy(nodes[src].name, leaf, TMPFS_MAX_NAME);
    inodes[nodes[dparent].ino].mtime = tmpfs_now();
    NI(src)->ctime = tmpfs_now();
    return 0;
}

static int tmpfs_rmdir(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 0, 0, 1);
    if (idx < 0) return idx;
    if (idx == 0) return -EBUSY;
    if (tmpfs_has_children(idx)) return -ENOTEMPTY;
    tmpfs_drop_node(idx);
    return 0;
}

static int tmpfs_seek(struct vfs_filesystem *fs, void *handle, int32_t offset, int whence) {
    (void)fs;
    tmpfs_handle_t *h = (tmpfs_handle_t *)handle;
    tmpfs_inode_t *n = tmpfs_hin(handle);
    if (!n) return -EBADF;
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
    tmpfs_inode_t *n = tmpfs_hin(handle);
    if (!n) return -EBADF;
    return (int)n->size;
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
        entries[count].inode = (uint32_t)nodes[i].ino + 1;
        entries[count].type = tmpfs_isdir(i) ? VFS_DIRECTORY : VFS_FILE;
        entries[count].lnk = tmpfs_islnk(i) ? 1 : 0;
        entries[count].size = NI(i)->size;
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
        if (tmpfs_has_children(idx)) return -EBUSY;
    }
    if (idx == 0) return -EBUSY;
    tmpfs_drop_node(idx);
    return 0;
}

static int tmpfs_exists(struct vfs_filesystem *fs, const char *rel_path) {
    (void)fs;
    if (!rel_path || !rel_path[0]) return 1;
    return tmpfs_lookup_any(rel_path) >= 0 ? 1 : 0;
}

/* lstat semantics: a symlink reports itself (the VFS resolver follows) */
static int tmpfs_stat(struct vfs_filesystem *fs, const char *rel_path, struct vfs_stat *st) {
    (void)fs;
    if (!st) return -EINVAL;
    int idx = tmpfs_lookup_any(rel_path);
    if (idx < 0) return idx;
    tmpfs_inode_t *in = NI(idx);
    memset(st, 0, sizeof(*st));
    st->ino = (uint64_t)nodes[idx].ino + 1;
    st->mode = in->mode;
    st->uid = in->uid;
    st->gid = in->gid;
    st->blksize = 512;
    st->mtime = in->mtime;
    st->ctime = in->ctime;
    st->atime = in->mtime;
    if (tmpfs_isdir(idx)) {
        uint32_t nl = 2;
        for (int i = 1; i < TMPFS_MAX_NODES; i++)
            if (nodes[i].used && nodes[i].parent == idx && tmpfs_isdir(i)) nl++;
        st->nlink = nl;
        st->size = 0;
    } else {
        st->nlink = in->nlink ? in->nlink : 1;
        st->size = in->size;
        st->blocks = tmpfs_islnk(idx) ? 0 : ((uint64_t)in->size + 511) / 512;
    }
    return 0;
}

static int tmpfs_truncate(struct vfs_filesystem *fs, const char *rel_path, uint64_t len) {
    (void)fs;
    int idx = tmpfs_lookup(rel_path, 0, 0, 0);
    if (idx < 0) return idx;
    if (tmpfs_isdir(idx)) return -EISDIR;
    if (tmpfs_islnk(idx)) return -EINVAL;
    return tmpfs_set_len(NI(idx), len);
}

static int tmpfs_ftruncate(struct vfs_filesystem *fs, void *handle, uint64_t len) {
    (void)fs;
    tmpfs_inode_t *n = tmpfs_hin(handle);
    if (!n) return -EBADF;
    return tmpfs_set_len(n, len);
}

static int tmpfs_readlink(struct vfs_filesystem *fs, const char *rel_path, char *buf, uint32_t cap) {
    (void)fs;
    if (!rel_path || !rel_path[0]) return -EINVAL;
    int idx = tmpfs_lookup_any(rel_path);
    if (idx < 0) return idx;
    if (!tmpfs_islnk(idx) || !NI(idx)->target) return -EINVAL;
    uint32_t n = NI(idx)->size;
    if (n > cap) n = cap;
    memcpy(buf, NI(idx)->target, n);
    return (int)n;
}

static int tmpfs_symlink(struct vfs_filesystem *fs, const char *rel_path, const char *target) {
    (void)fs;
    size_t tl = strlen(target);
    if (!tl) return -ENOENT;
    if (tl > TMPFS_LINK_MAX) return -ENAMETOOLONG;
    char leaf[TMPFS_MAX_NAME];
    int par = tmpfs_parent(rel_path, leaf);
    if (par < 0) return par;
    if (tmpfs_find_child(par, leaf) >= 0) return -EEXIST;
    char *t = (char *)kalloc(tl + 1);
    if (!t) return -ENOSPC;
    memcpy(t, target, tl + 1);
    int ino = tmpfs_alloc_inode(T_IFLNK | 0777);
    if (ino < 0) { kfree(t); return -ENOSPC; }
    inodes[ino].target = t;
    inodes[ino].size = (uint32_t)tl;
    int r = tmpfs_add_node(par, leaf, ino);
    if (r < 0) { inodes[ino].nlink = 0; tmpfs_put_inode(ino); return r; }
    return 0;
}

static int tmpfs_link(struct vfs_filesystem *fs, const char *old_rel, const char *new_rel) {
    (void)fs;
    int src = tmpfs_lookup_any(old_rel);
    if (src < 0) return src;
    if (tmpfs_isdir(src)) return -EPERM;
    char leaf[TMPFS_MAX_NAME];
    int par = tmpfs_parent(new_rel, leaf);
    if (par < 0) return par;
    if (tmpfs_find_child(par, leaf) >= 0) return -EEXIST;
    int r = tmpfs_add_node(par, leaf, nodes[src].ino);
    if (r < 0) return r;
    NI(src)->nlink++;
    NI(src)->ctime = tmpfs_now();
    return 0;
}

static int tmpfs_setattr(struct vfs_filesystem *fs, const char *rel_path, int what,
                         uint32_t mode, uint32_t uid, uint32_t gid) {
    (void)fs;
    int idx = tmpfs_lookup_any(rel_path);
    if (idx < 0) return idx;
    tmpfs_inode_t *in = NI(idx);
    if ((what & VFS_SA_MODE) && !tmpfs_islnk(idx)) in->mode = (in->mode & T_IFMT) | (mode & 07777);
    if (what & VFS_SA_OWNER) {
        if (uid != (uint32_t)-1) in->uid = uid;
        if (gid != (uint32_t)-1) in->gid = gid;
        if (!tmpfs_islnk(idx) && !tmpfs_isdir(idx)) in->mode &= ~06000u;   /* chown clears setuid/setgid */
    }
    in->ctime = tmpfs_now();
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
    .lstat = tmpfs_stat,
    .truncate = tmpfs_truncate,
    .ftruncate = tmpfs_ftruncate,
    .rename = tmpfs_rename,
    .rmdir = tmpfs_rmdir,
    .readlink = tmpfs_readlink,
    .symlink = tmpfs_symlink,
    .link = tmpfs_link,
    .setattr = tmpfs_setattr,
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
