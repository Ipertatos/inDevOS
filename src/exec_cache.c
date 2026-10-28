/* Exec image cache -- see exec_cache.h. */
#include "exec_cache.h"
#include "vfs.h"
#include "mem.h"
#include "string.h"
#include "atomic.h"
#include "serial.h"
#include "../include/indevos_errno.h"

#define EXC_SLOTS      8
#define EXC_PATH       256
#define EXC_MAX_TOTAL  (16u << 20)
#define EXC_MAX_FILE   (8u << 20)
#define EXC_LOADING    4                 /* concurrent uncached loads tracked */

/* Entries are heap objects: a reader holds the entry pointer (refs) while it
 * copies the image outside the lock; an entry dropped from the table while
 * referenced is marked dead and freed by its last reader (exc_put). */
typedef struct exc_entry {
    uint32_t refs;                       /* copies in progress */
    bool     dead;                       /* no longer in the table */
    char     path[EXC_PATH];
    uint32_t size;
    int64_t  mtime;
    uint64_t ino;
    bool     have_ino;
    uint64_t lru;
    uint8_t *data;
} exc_entry_t;

typedef struct {
    bool used;
    bool dirty;                          /* invalidated while being read */
    char path[EXC_PATH];
} exc_loading_t;

static spinlock_t     exc_lock = {0};
static exc_entry_t   *exc[EXC_SLOTS];
static exc_loading_t  exc_ld[EXC_LOADING];
static uint64_t       exc_total, exc_tick, exc_hits, exc_misses;
static volatile uint32_t exc_active;     /* entries + loads: hooks skip when 0 */

extern uint64_t total_memory_bytes;

static uint64_t exc_budget(void) {
    uint64_t b = total_memory_bytes / 32;
    if (b > EXC_MAX_TOTAL) b = EXC_MAX_TOTAL;
    return b;
}

static char exc_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive (FAT names) prefix compare: @a starts with @pre and the
 * next char ends a component (or @exact requires full equality). */
static bool exc_match(const char *a, const char *pre, bool tree) {
    size_t i = 0;
    for (; pre[i]; i++)
        if (exc_lower(a[i]) != exc_lower(pre[i])) return false;
    if (a[i] == '\0') return true;
    return tree && (a[i] == '/' || (i > 0 && pre[i - 1] == '/'));
}

/* Lexical normalisation: collapse "//", "/./", "/x/../"; no symlinks. */
static bool exc_norm(const char *in, char *out, size_t cap) {
    if (!in || in[0] != '/') return false;
    size_t o = 0;
    const char *s = in;
    for (;;) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t cl = (size_t)(e - s);
        if (cl == 1 && s[0] == '.') { s = e; continue; }
        if (cl == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            s = e; continue;
        }
        if (o + 1 + cl + 1 > cap) return false;
        out[o++] = '/';
        memcpy(out + o, s, cl); o += cl;
        s = e;
    }
    if (o == 0) out[o++] = '/';
    out[o] = '\0';
    return true;
}

static void exc_recount(void) {
    uint32_t n = 0;
    for (int i = 0; i < EXC_SLOTS; i++) if (exc[i]) n++;
    for (int i = 0; i < EXC_LOADING; i++) if (exc_ld[i].used) n++;
    __atomic_store_n(&exc_active, n, __ATOMIC_RELEASE);
}

/* Drop slot @i (lock held).  Data is freed now, or by the last reader. */
static void exc_drop_locked(int i) {
    exc_entry_t *e = exc[i];
    if (!e) return;
    exc[i] = NULL;
    exc_total -= e->size;
    if (e->refs == 0) {
        kfree(e->data);
        kfree(e);
    } else {
        e->dead = true;                  /* last exc_put frees it */
    }
}

static void exc_invalidate_locked(const char *norm, bool tree) {
    for (int i = 0; i < EXC_SLOTS; i++)
        if (exc[i] && exc_match(exc[i]->path, norm, tree)) exc_drop_locked(i);
    for (int i = 0; i < EXC_LOADING; i++)
        if (exc_ld[i].used && exc_match(exc_ld[i].path, norm, tree)) exc_ld[i].dirty = true;
    exc_recount();
}

static void exc_invalidate_common(const char *path, bool tree) {
    if (!__atomic_load_n(&exc_active, __ATOMIC_ACQUIRE) || !path) return;
    char n[EXC_PATH];
    klock(&exc_lock);
    if (exc_norm(path, n, sizeof n)) {
        exc_invalidate_locked(n, tree);
    } else {
        /* Unparseable/too long path: be safe and drop everything. */
        for (int i = 0; i < EXC_SLOTS; i++) exc_drop_locked(i);
        for (int i = 0; i < EXC_LOADING; i++) if (exc_ld[i].used) exc_ld[i].dirty = true;
        exc_recount();
    }
    kunlock(&exc_lock);
}

void exec_cache_invalidate(const char *path)      { exc_invalidate_common(path, false); }
void exec_cache_invalidate_tree(const char *path) { exc_invalidate_common(path, true); }

void exec_cache_flush(void) {
    if (!__atomic_load_n(&exc_active, __ATOMIC_ACQUIRE)) return;
    klock(&exc_lock);
    for (int i = 0; i < EXC_SLOTS; i++) exc_drop_locked(i);
    for (int i = 0; i < EXC_LOADING; i++) if (exc_ld[i].used) exc_ld[i].dirty = true;
    exc_recount();
    kunlock(&exc_lock);
}

void exec_cache_stats(uint32_t *entries, uint64_t *bytes, uint64_t *hits, uint64_t *misses) {
    klock(&exc_lock);
    uint32_t n = 0;
    for (int i = 0; i < EXC_SLOTS; i++) if (exc[i]) n++;
    if (entries) *entries = n;
    if (bytes) *bytes = exc_total;
    if (hits) *hits = exc_hits;
    if (misses) *misses = exc_misses;
    kunlock(&exc_lock);
}

/* Identity of the file behind an open fd: size from the handle, mtime from
 * the fs (times op), inode/nlink when the fs implements stat. */
typedef struct { uint32_t size; int64_t mtime; uint64_t ino; bool have_ino; uint32_t nlink; } exc_id_t;

static int exc_identify(const char *path, int fd, exc_id_t *id) {
    memset(id, 0, sizeof(*id));
    id->nlink = 1;
    int size = vfs_seek(fd, 0, VFS_SEEK_END);
    if (size < 0) return -EIO;
    vfs_seek(fd, 0, VFS_SEEK_SET);
    id->size = (uint32_t)size;
    vfs_stat_t st;
    if (vfs_stat(path, &st) == 0) {
        id->mtime = st.mtime;
        id->ino = st.ino;
        id->have_ino = true;
        id->nlink = st.nlink ? st.nlink : 1;
        if (st.size != (uint64_t)size) return -EAGAIN;   /* changing under us */
        return 0;
    }
    int64_t mt = 0;
    if (vfs_times(path, 0, &mt, NULL) == 0) id->mtime = mt;
    else id->mtime = -1;                                 /* unknown: never cached */
    return 0;
}

static void exc_put(exc_entry_t *e) {
    klock(&exc_lock);
    if (e->refs) e->refs--;
    bool free_it = e->dead && e->refs == 0;
    kunlock(&exc_lock);
    if (free_it) { kfree(e->data); kfree(e); }
}

int exec_image_load(const char *path, void **out_buf) {
    if (!path || !out_buf) return -EINVAL;
    *out_buf = NULL;

    char key[EXC_PATH];
    bool cacheable = false;
    if (path[0] == '/') {
        char *rbuf = (char *)kalloc(VFS_MAX_PATH);      /* keep the exec stack small */
        if (rbuf && vfs_resolve(path, rbuf, VFS_MAX_PATH, true) == 0 &&
            exc_norm(rbuf, key, sizeof key))
            cacheable = true;
        if (rbuf) kfree(rbuf);
    }

    /* Open/stat the caller's path exactly as vfs_read_file_alloc() did; the
     * resolved, normalised @key only names the cache entry. */
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return fd;

    exc_id_t id;
    int ir = exc_identify(path, fd, &id);
    if (ir == -EIO) { vfs_close(fd); return -EIO; }
    if (ir != 0 || id.mtime < 0 || id.nlink > 1 || id.size == 0 ||
        id.size > EXC_MAX_FILE || (uint64_t)id.size > exc_budget())
        cacheable = false;

    /* Hit? */
    if (cacheable) {
        exc_entry_t *hit = NULL;
        klock(&exc_lock);
        for (int i = 0; i < EXC_SLOTS; i++) {
            exc_entry_t *e = exc[i];
            if (!e || strcmp(e->path, key) != 0) continue;
            if (e->size == id.size && e->mtime == id.mtime &&
                e->have_ino == id.have_ino && (!id.have_ino || e->ino == id.ino)) {
                e->refs++;
                e->lru = ++exc_tick;
                exc_hits++;
                hit = e;
            } else {
                exc_drop_locked(i);                  /* stale identity */
            }
            break;
        }
        exc_recount();
        kunlock(&exc_lock);
        if (hit) {
            vfs_close(fd);
            uint8_t *copy = (uint8_t *)kalloc((size_t)id.size + 1);
            if (!copy) { exc_put(hit); return -ENOMEM; }
            memcpy(copy, hit->data, id.size);
            copy[id.size] = '\0';
            exc_put(hit);
            *out_buf = copy;
            return (int)id.size;
        }
    }

    /* Miss: read the file, registering the load so a concurrent write marks it. */
    int ld = -1;
    if (cacheable) {
        klock(&exc_lock);
        exc_misses++;
        for (int i = 0; i < EXC_LOADING; i++)
            if (!exc_ld[i].used) { ld = i; break; }
        if (ld >= 0) {
            exc_ld[ld].used = true;
            exc_ld[ld].dirty = false;
            memcpy(exc_ld[ld].path, key, strlen(key) + 1);
        }
        exc_recount();
        kunlock(&exc_lock);
    }

    uint8_t *buf = (uint8_t *)kalloc((size_t)id.size + 1);
    if (!buf) {
        vfs_close(fd);
        if (ld >= 0) { klock(&exc_lock); exc_ld[ld].used = false; exc_recount(); kunlock(&exc_lock); }
        return -ENOMEM;
    }
    uint32_t got = 0;
    while (got < id.size) {
        int n = vfs_read(fd, buf + got, id.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    vfs_close(fd);
    buf[got] = '\0';
    *out_buf = buf;

    if (ld < 0) return (int)got;

    /* Insert a second copy if nothing changed while we read it. */
    uint8_t *keep = NULL;
    exc_entry_t *ent = NULL;
    if (got == id.size) {
        keep = (uint8_t *)kalloc(id.size);
        ent = (exc_entry_t *)kalloc(sizeof(exc_entry_t));
        if (!keep || !ent) { if (keep) kfree(keep); if (ent) kfree(ent); keep = NULL; ent = NULL; }
    }
    if (keep) memcpy(keep, buf, id.size);

    klock(&exc_lock);
    bool ok = keep && !exc_ld[ld].dirty;
    exc_ld[ld].used = false;
    if (ok) {
        for (int i = 0; i < EXC_SLOTS; i++)        /* raced with another loader */
            if (exc[i] && strcmp(exc[i]->path, key) == 0) exc_drop_locked(i);
        uint64_t budget = exc_budget();
        while (exc_total + id.size > budget) {     /* evict LRU */
            int v = -1;
            for (int i = 0; i < EXC_SLOTS; i++)
                if (exc[i] && (v < 0 || exc[i]->lru < exc[v]->lru)) v = i;
            if (v < 0) break;
            exc_drop_locked(v);
        }
        int slot = -1;
        for (int i = 0; i < EXC_SLOTS; i++) if (!exc[i]) { slot = i; break; }
        if (slot < 0) {
            int v = 0;
            for (int i = 1; i < EXC_SLOTS; i++) if (exc[i]->lru < exc[v]->lru) v = i;
            exc_drop_locked(v);
            slot = v;
        }
        if (exc_total + id.size <= budget && ent) {
            exc_entry_t *e = ent; ent = NULL;
            exc[slot] = e;
            e->refs = 0; e->dead = false;
            memcpy(e->path, key, strlen(key) + 1);
            e->size = id.size; e->mtime = id.mtime; e->ino = id.ino; e->have_ino = id.have_ino;
            e->lru = ++exc_tick;
            e->data = keep;
            exc_total += id.size;
            keep = NULL;
        }
    }
    exc_recount();
    kunlock(&exc_lock);
    if (keep) kfree(keep);
    if (ent) kfree(ent);
    return (int)got;
}
