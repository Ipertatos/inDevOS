/**
 * @file inotify.c
 * @brief Linux-compatible inotify over VFS path hooks.
 *
 * Watches are kept by normalised absolute path (Linux keys them by inode;
 * renames of watched objects rewrite the stored paths so a watch follows
 * its object).  vfs.c reports every change once, after the filesystem
 * operation succeeded, so tmpfs, FAT32 and exFAT behave the same.
 *
 * Event order matches Linux: for modify/close/open/access the parent
 * directory watch is queued before the watch on the object itself; for
 * unlink/rmdir the object's IN_DELETE_SELF + IN_IGNORED come before the
 * parent's IN_DELETE.  An event identical to the last unread one in the
 * queue is merged into it, as Linux does.
 */
#include "inotify.h"
#include "atomic.h"
#include "mem.h"
#include "string.h"
#include "../include/indevos_errno.h"
#include "kpoll.h"

#define INO_MAX_INST   32
#define INO_MAX_WATCH  128
#define INO_PATH       256
#define INO_QBYTES     16384

typedef struct {
    int32_t  wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
} ino_ev_t;

typedef struct {
    int      wd;                /* 0 = free slot */
    uint32_t mask;
    bool     is_dir;
    char     path[INO_PATH];
} ino_watch_t;

typedef struct ino_inst {
    int         next_wd;
    uint32_t    gen;            /* bumped per queued event (EPOLLET edges) */
    uint32_t    head, tail;     /* queue: bytes [head, tail) */
    uint32_t    last;           /* offset of the last queued event */
    bool        has_last;
    bool        overflowed;
    ino_watch_t w[INO_MAX_WATCH];
    uint8_t     q[INO_QBYTES];
} ino_inst_t;

volatile int inotify_active;
static ino_inst_t *ino_all[INO_MAX_INST];
static spinlock_t  ino_lock = {0};
static uint32_t    ino_cookie;
static uint32_t    ino_pushed;          /* c18: events queued (wake only then) */

/* Collapse "//", "." and ".."; drop a trailing '/'. */
static void ino_norm(const char *in, char *out) {
    size_t o = 0;
    out[o++] = '/';
    const char *s = in;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - s);
        if (n == 1 && s[0] == '.') {
            /* skip */
        } else if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; if (o > 1) o--; }
        } else if (o + n + 2 < INO_PATH) {
            if (o > 1) out[o++] = '/';
            memcpy(out + o, s, n);
            o += n;
        }
        s = e;
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = '\0';
}

static bool ino_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* "/a/b/c" -> dir "/a/b", base "c"; "/x" -> "/", "x"; "/" -> "", "". */
static void ino_split(const char *p, char *dir, const char **base) {
    size_t n = strlen(p), i = n;
    while (i > 0 && p[i - 1] != '/') i--;
    *base = p + i;
    if (n <= 1) { dir[0] = '\0'; *base = p + n; return; }
    if (i <= 1) { dir[0] = '/'; dir[1] = '\0'; return; }
    memcpy(dir, p, i - 1);
    dir[i - 1] = '\0';
}

static uint32_t ino_used(const ino_inst_t *in) { return in->tail - in->head; }

static void ino_compact(ino_inst_t *in) {
    if (in->head == 0) return;
    uint32_t n = ino_used(in);
    memmove(in->q, in->q + in->head, n);
    if (in->has_last) in->last -= in->head;
    in->tail = n;
    in->head = 0;
}

static void ino_push(ino_inst_t *in, int wd, uint32_t mask, uint32_t cookie, const char *name) {
    size_t nl = name ? strlen(name) : 0;
    uint32_t len = nl ? (uint32_t)((nl + 1 + 15) & ~(size_t)15) : 0;
    /* merge with an identical last unread event */
    if (in->has_last && in->last >= in->head) {
        ino_ev_t *l = (ino_ev_t *)(in->q + in->last);
        if (l->wd == wd && l->mask == mask && l->cookie == cookie && l->len == len &&
            (len == 0 || ino_streq((const char *)(l + 1), name)))
            return;
    }
    uint32_t need = (uint32_t)sizeof(ino_ev_t) + len;
    if (in->tail + need > INO_QBYTES) ino_compact(in);
    if (in->tail + need + sizeof(ino_ev_t) > INO_QBYTES) {
        /* keep room for one IN_Q_OVERFLOW */
        if (in->overflowed || in->tail + sizeof(ino_ev_t) > INO_QBYTES) return;
        in->overflowed = true;
        wd = -1; mask = IN_Q_OVERFLOW; cookie = 0; len = 0; nl = 0;
        need = sizeof(ino_ev_t);
    }
    ino_ev_t *e = (ino_ev_t *)(in->q + in->tail);
    e->wd = wd; e->mask = mask; e->cookie = cookie; e->len = len;
    if (len) {
        memset((char *)(e + 1), 0, len);
        memcpy((char *)(e + 1), name, nl);
    }
    in->last = in->tail;
    in->has_last = true;
    in->tail += need;
    ino_pushed++;
    __atomic_add_fetch(&in->gen, 1, __ATOMIC_RELEASE);
}

static void ino_drop_watch(ino_inst_t *in, ino_watch_t *w) {
    ino_push(in, w->wd, IN_IGNORED, 0, NULL);
    w->wd = 0;
}

/* Deliver mask m on watch w (name NULL for self events). */
static void ino_deliver(ino_inst_t *in, ino_watch_t *w, uint32_t m, uint32_t cookie, const char *name) {
    if (!(w->mask & m & IN_ALL_EVENTS)) return;
    ino_push(in, w->wd, m, cookie, name);
    if (w->mask & IN_ONESHOT) ino_drop_watch(in, w);
}

/* Self events: the watched object itself. */
static void ino_self(const char *np, uint32_t mask, uint32_t cookie) {
    uint32_t ev = mask & IN_ALL_EVENTS;
    uint32_t sm;
    if (ev & IN_DELETE)          sm = IN_DELETE_SELF;
    else if (ev & IN_MOVED_FROM) sm = IN_MOVE_SELF;
    else if (ev & (IN_CREATE | IN_MOVED_TO)) return;
    else                         sm = ev;
    for (int i = 0; i < INO_MAX_INST; i++) {
        ino_inst_t *in = ino_all[i];
        if (!in) continue;
        for (int k = 0; k < INO_MAX_WATCH; k++) {
            ino_watch_t *w = &in->w[k];
            if (!w->wd || !ino_streq(w->path, np)) continue;
            uint32_t m = sm;
            if (w->is_dir && !(sm & (IN_DELETE_SELF | IN_MOVE_SELF))) m |= IN_ISDIR;
            ino_deliver(in, w, m, 0, NULL);
            if (w->wd && (sm & IN_DELETE_SELF)) ino_drop_watch(in, w);
        }
    }
}

static void ino_parent(const char *dir, const char *base, uint32_t mask, uint32_t cookie) {
    if (!dir[0]) return;
    for (int i = 0; i < INO_MAX_INST; i++) {
        ino_inst_t *in = ino_all[i];
        if (!in) continue;
        for (int k = 0; k < INO_MAX_WATCH; k++) {
            ino_watch_t *w = &in->w[k];
            if (w->wd && w->is_dir && ino_streq(w->path, dir))
                ino_deliver(in, w, mask, cookie, base);
        }
    }
}

static void ino_event_locked(const char *np, uint32_t mask, uint32_t cookie) {
    char dir[INO_PATH];
    const char *base;
    ino_split(np, dir, &base);
    if (mask & IN_DELETE) {
        ino_self(np, mask, cookie);
        ino_parent(dir, base, mask, cookie);
    } else {
        ino_parent(dir, base, mask, cookie);
        ino_self(np, mask, cookie);
    }
}

void inotify_vfs_event(const char *path, uint32_t mask) {
    if (!inotify_active || !path) return;
    char np[INO_PATH];
    ino_norm(path, np);
    klock(&ino_lock);
    uint32_t before = ino_pushed;
    ino_event_locked(np, mask, 0);
    bool pushed = ino_pushed != before;
    kunlock(&ino_lock);
    if (pushed) kpoll_wake();
}

void inotify_vfs_rename(const char *oldpath, const char *newpath, bool is_dir) {
    if (!inotify_active || !oldpath || !newpath) return;
    char o[INO_PATH], n[INO_PATH];
    ino_norm(oldpath, o);
    ino_norm(newpath, n);
    uint32_t d = is_dir ? IN_ISDIR : 0;
    klock(&ino_lock);
    uint32_t ck = ++ino_cookie;
    if (!ck) ck = ++ino_cookie;
    char od[INO_PATH], nd[INO_PATH];
    const char *ob, *nb;
    ino_split(o, od, &ob);
    ino_split(n, nd, &nb);
    ino_parent(od, ob, IN_MOVED_FROM | d, ck);
    ino_parent(nd, nb, IN_MOVED_TO | d, ck);
    ino_self(o, IN_MOVED_FROM, 0);
    /* watches on the moved object (and below it) follow it */
    size_t ol = strlen(o), nl = strlen(n);
    for (int i = 0; i < INO_MAX_INST; i++) {
        ino_inst_t *in = ino_all[i];
        if (!in) continue;
        for (int k = 0; k < INO_MAX_WATCH; k++) {
            ino_watch_t *w = &in->w[k];
            if (!w->wd) continue;
            if (strncmp(w->path, o, ol) != 0 || (w->path[ol] != '\0' && w->path[ol] != '/')) continue;
            size_t rest = strlen(w->path + ol);
            if (nl + rest >= INO_PATH) continue;
            char tmp[INO_PATH];
            memcpy(tmp, n, nl);
            memcpy(tmp + nl, w->path + ol, rest + 1);
            memcpy(w->path, tmp, nl + rest + 1);
        }
    }
    kunlock(&ino_lock);
    kpoll_wake();
}

void *inotify_create(void) {
    ino_inst_t *in = (ino_inst_t *)kalloc(sizeof(ino_inst_t));
    if (!in) return NULL;
    memset(in, 0, sizeof(*in));
    in->next_wd = 1;
    klock(&ino_lock);
    int slot = -1;
    for (int i = 0; i < INO_MAX_INST; i++) if (!ino_all[i]) { slot = i; break; }
    if (slot < 0) { kunlock(&ino_lock); kfree(in); return NULL; }
    ino_all[slot] = in;
    inotify_active++;
    kunlock(&ino_lock);
    return in;
}

void inotify_release(void *h) {
    if (!h) return;
    klock(&ino_lock);
    for (int i = 0; i < INO_MAX_INST; i++)
        if (ino_all[i] == h) { ino_all[i] = NULL; inotify_active--; }
    kunlock(&ino_lock);
    kfree(h);
}

int inotify_add(void *h, const char *abspath, uint32_t mask, bool is_dir) {
    ino_inst_t *in = (ino_inst_t *)h;
    if (!in) return -EINVAL;
    if (!(mask & IN_ALL_EVENTS)) return -EINVAL;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)) return -EINVAL;
    if ((mask & IN_ONLYDIR) && !is_dir) return -ENOTDIR;
    char np[INO_PATH];
    ino_norm(abspath, np);
    uint32_t keep = (mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK));
    klock(&ino_lock);
    for (int k = 0; k < INO_MAX_WATCH; k++) {
        ino_watch_t *w = &in->w[k];
        if (w->wd && ino_streq(w->path, np)) {
            if (mask & IN_MASK_CREATE) { kunlock(&ino_lock); return -EEXIST; }
            w->mask = (mask & IN_MASK_ADD) ? (w->mask | keep) : keep;
            int wd = w->wd;
            kunlock(&ino_lock);
            return wd;
        }
    }
    for (int k = 0; k < INO_MAX_WATCH; k++) {
        ino_watch_t *w = &in->w[k];
        if (w->wd) continue;
        w->wd = in->next_wd++;
        w->mask = keep;
        w->is_dir = is_dir;
        memcpy(w->path, np, strlen(np) + 1);
        int wd = w->wd;
        kunlock(&ino_lock);
        return wd;
    }
    kunlock(&ino_lock);
    return -ENOSPC;
}

int inotify_rm(void *h, int wd) {
    ino_inst_t *in = (ino_inst_t *)h;
    if (!in || wd <= 0) return -EINVAL;
    klock(&ino_lock);
    for (int k = 0; k < INO_MAX_WATCH; k++) {
        if (in->w[k].wd == wd) {
            ino_drop_watch(in, &in->w[k]);
            kunlock(&ino_lock);
            kpoll_wake();
            return 0;
        }
    }
    kunlock(&ino_lock);
    return -EINVAL;
}

long inotify_take(void *h, void *kbuf, size_t len) {
    ino_inst_t *in = (ino_inst_t *)h;
    if (!in) return -EINVAL;
    klock(&ino_lock);
    size_t got = 0;
    while (in->head < in->tail) {
        ino_ev_t *e = (ino_ev_t *)(in->q + in->head);
        uint32_t sz = (uint32_t)sizeof(ino_ev_t) + e->len;
        if (got + sz > len) break;
        memcpy((char *)kbuf + got, e, sz);
        got += sz;
        in->head += sz;
        if (in->has_last && in->last < in->head) in->has_last = false;
        if (e->mask == IN_Q_OVERFLOW) in->overflowed = false;
    }
    if (in->head == in->tail) { in->head = in->tail = 0; in->has_last = false; }
    bool more = in->head < in->tail;
    kunlock(&ino_lock);
    if (!got && more) return -EINVAL;
    return (long)got;
}

bool inotify_ready(void *h) {
    ino_inst_t *in = (ino_inst_t *)h;
    return in && __atomic_load_n(&in->tail, __ATOMIC_ACQUIRE) != __atomic_load_n(&in->head, __ATOMIC_ACQUIRE);
}

uint32_t inotify_gen(void *h) {
    ino_inst_t *in = (ino_inst_t *)h;
    return in ? __atomic_load_n(&in->gen, __ATOMIC_ACQUIRE) : 0;
}

uint32_t inotify_pending_bytes(void *h) {
    ino_inst_t *in = (ino_inst_t *)h;
    if (!in) return 0;
    klock(&ino_lock);
    uint32_t n = ino_used(in);
    kunlock(&ino_lock);
    return n;
}
