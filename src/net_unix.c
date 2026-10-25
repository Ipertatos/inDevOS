/*
 * AF_UNIX sockets for the Linux ABI layer.
 *
 *  - socketpair(SOCK_STREAM | SOCK_DGRAM | SOCK_SEQPACKET)
 *  - bind/listen/connect/accept on filesystem paths and abstract names
 *  - stream / datagram / seqpacket send/recv, shutdown, poll
 *  - SCM_RIGHTS descriptor passing, SO_PEERCRED
 *
 * Names live in an in-kernel registry for the lifetime of the bound socket;
 * no socket inode is created on disk (stat() on the path fails, unlink() of
 * it is not needed). Everything is serialised by one spinlock; user memory
 * is only touched outside it.
 */
#include "syscall_internal.h"
#include "mem.h"
#include "string.h"
#include "thread.h"
#include "uaccess.h"
#include "atomic.h"
#include "../include/indevos_errno.h"

#define AF_UNIX_      1
#ifndef EPROTOTYPE
#define EPROTOTYPE    91
#endif
#define UX_STREAM     1
#define UX_DGRAM      2
#define UX_SEQPACKET  5
#define UX_RXMAX      (256u * 1024u)
#define UX_MSGMAX     65536u
#define UX_MAXQ       512
#define UX_BACKLOG    64
#define UX_MAXFDS     32
#define UX_NAMEMAX    110

#define UXS_NEW       0
#define UXS_LISTEN    1
#define UXS_CONN      2

#define MSG_PEEK_     0x2
#define MSG_TRUNC_    0x20
#define MSG_CTRUNC_   0x8
#define MSG_DONTWAIT_ 0x40
#define MSG_WAITALL_  0x100
#define MSG_NOSIGNAL_ 0x4000
#define MSG_CMSG_CLOEXEC_ 0x40000000

typedef struct ux_msg {
    struct ux_msg *next;
    uint32_t len, off;
    int nfds;
    file_descriptor_t *fds;
    int fromlen;
    char from[UX_NAMEMAX];
    uint8_t data[];
} ux_msg_t;

typedef struct ux_ep {
    struct ux_ep *all_next;
    int type, state;
    struct ux_ep *peer;
    bool peer_gone;          /* peer closed: EOF + POLLHUP (stream) / ECONNREFUSED (dgram) */
    bool rd_eof;             /* peer did SHUT_WR (or closed) */
    bool shut_rd, shut_wr;
    ux_msg_t *rxh, *rxt;
    uint32_t rx_bytes, rx_count;
    int nlen;
    bool reg;                /* name is in the registry (bound socket) */
    char name[UX_NAMEMAX];   /* registry key: resolved absolute path or abstract */
    int dlen;                /* name as the caller spelled it (getsockname etc.) */
    char dname[UX_NAMEMAX];
    struct ux_ep *bl[UX_BACKLOG];
    int nbl, backlog;
    struct ux_ep *listener;  /* pending (not yet accepted) server end */
    uint32_t pid, peer_pid;
} ux_ep_t;

static spinlock_t ux_lock;
static ux_ep_t *ux_all;
static uint32_t ux_autobind_ctr;

extern int  allocate_fd(process_t *process);
extern uint32_t *fd_refcount_create(void);
extern int  fd_flight_get(process_t *proc, int fd, file_descriptor_t *out);
extern int  fd_flight_install(process_t *proc, const file_descriptor_t *f, bool cloexec);
extern void fd_flight_release(file_descriptor_t *f);
extern void proc_raise_sigpipe(process_t *proc);
extern bool proc_kill_pending(process_t *p);
extern long lx_unix_parse_addr(process_t *p, const void *uaddr, uint32_t alen,
                               char *out, int *outlen);

static inline ux_ep_t *UX(kernel_socket_t *s) { return (ux_ep_t *)s->transport; }
/* lock held: name to report to user space */
static int ux_dn(const ux_ep_t *e, char *out) {
    const char *n = e->dlen ? e->dname : e->name;
    int l = e->dlen ? e->dlen : e->nlen;
    if (l) memcpy(out, n, (size_t)l);
    return l;
}

static ux_ep_t *ux_ep_new(int type, process_t *p) {
    ux_ep_t *e = (ux_ep_t *)kalloc(sizeof(ux_ep_t));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->pid = p ? p->pid : 0;
    klock(&ux_lock);
    e->all_next = ux_all;
    ux_all = e;
    kunlock(&ux_lock);
    return e;
}

/* lock held */
static ux_ep_t *ux_lookup(const char *name, int nlen) {
    if (nlen <= 0) return NULL;
    for (ux_ep_t *e = ux_all; e; e = e->all_next)
        if (e->reg && e->nlen == nlen && memcmp(e->name, name, (size_t)nlen) == 0)
            return e;
    return NULL;
}

static void ux_msg_free(ux_msg_t *m) {
    if (!m) return;
    if (m->fds) {
        for (int i = 0; i < m->nfds; i++) fd_flight_release(&m->fds[i]);
        kfree(m->fds);
    }
    kfree(m);
}

/* lock held: detach ep from everything; returns the rx queue + pending
 * server ends to be freed after unlocking. */
static void ux_detach_locked(ux_ep_t *e, ux_msg_t **msgs, ux_ep_t **pend, int *npend) {
    ux_ep_t **pp = &ux_all;
    while (*pp) {
        if (*pp == e) { *pp = e->all_next; break; }
        pp = &(*pp)->all_next;
    }
    for (ux_ep_t *x = ux_all; x; x = x->all_next) {
        if (x->peer == e) {
            x->peer = NULL;
            x->peer_gone = true;
            x->rd_eof = true;
        }
    }
    for (int i = 0; i < e->nbl; i++) pend[(*npend)++] = e->bl[i];
    e->nbl = 0;
    *msgs = e->rxh;
    e->rxh = e->rxt = NULL;
    e->rx_bytes = e->rx_count = 0;
    e->peer = NULL;
}

static void ux_destroy(ux_ep_t *e) {
    ux_ep_t *pend[UX_BACKLOG];
    int npend = 0;
    ux_msg_t *msgs = NULL;
    klock(&ux_lock);
    ux_detach_locked(e, &msgs, pend, &npend);
    kunlock(&ux_lock);
    while (msgs) { ux_msg_t *n = msgs->next; ux_msg_free(msgs); msgs = n; }
    for (int i = 0; i < npend; i++) ux_destroy(pend[i]);
    kfree(e);
}

void ux_release(kernel_socket_t *s) {
    if (s && s->transport) ux_destroy(UX(s));
    if (s) s->transport = NULL;
}

/* ── fd creation ─────────────────────────────────────────────────────────── */
static long ux_new_fd(process_t *p, ux_ep_t *e, long flags) {
    int fd = allocate_fd(p);
    if (fd < 0) return fd;
    kernel_socket_t *ks = (kernel_socket_t *)kalloc(sizeof(kernel_socket_t));
    uint32_t *rc = fd_refcount_create();
    if (!ks || !rc) {
        if (ks) kfree(ks);
        if (rc) kfree(rc);
        p->fds[fd].in_use = false;
        return -ENOMEM;
    }
    memset(ks, 0, sizeof(*ks));
    ks->domain = AF_UNIX_;
    ks->type = e->type;
    ks->transport = e;
    p->fds[fd].file_handle = ks;
    p->fds[fd].ref_count = rc;
    p->fds[fd].fd_type = FD_TYPE_SOCKET;
    p->fds[fd].flags = (flags & 04000) ? O_NONBLOCK : 0;
    p->fds[fd].cloexec = (flags & 02000000) != 0;
    p->fds[fd].position = 0;
    p->fds[fd].vfs_fd = -1;
    p->fds[fd].in_use = true;
    return fd;
}

static bool ux_type_ok(int t) { return t == UX_STREAM || t == UX_DGRAM || t == UX_SEQPACKET; }

long ux_socket(process_t *p, long type) {
    int t = (int)(type & 0xF);
    if (!ux_type_ok(t)) return -EPROTONOSUPPORT;
    ux_ep_t *e = ux_ep_new(t, p);
    if (!e) return -ENOMEM;
    long fd = ux_new_fd(p, e, type);
    if (fd < 0) ux_destroy(e);
    return fd;
}

long ux_socketpair(process_t *p, long type, int *usv) {
    int t = (int)(type & 0xF);
    if (!ux_type_ok(t)) return -EPROTONOSUPPORT;
    if (!usv || !access_ok_user_range((uint64_t)usv, 8)) return -EFAULT;
    ux_ep_t *a = ux_ep_new(t, p), *b = ux_ep_new(t, p);
    if (!a || !b) { if (a) ux_destroy(a); if (b) ux_destroy(b); return -ENOMEM; }
    klock(&ux_lock);
    a->peer = b; b->peer = a;
    a->state = b->state = UXS_CONN;
    a->peer_pid = b->peer_pid = p->pid;
    kunlock(&ux_lock);
    long fa = ux_new_fd(p, a, type);
    if (fa < 0) { ux_destroy(a); ux_destroy(b); return fa; }
    long fb = ux_new_fd(p, b, type);
    if (fb < 0) { sys_close((int)fa); ux_destroy(b); return fb; }
    int sv[2] = { (int)fa, (int)fb };
    if (copy_to_user(usv, sv, sizeof(sv))) {
        sys_close((int)fa); sys_close((int)fb);
        return -EFAULT;
    }
    return 0;
}

/* ── bind / listen / connect / accept ──────────────────────────────────── */
long ux_bind(process_t *p, kernel_socket_t *s, const void *uaddr, uint32_t alen) {
    char nm[UX_NAMEMAX];
    int nl = 0;
    long r = lx_unix_parse_addr(p, uaddr, alen, nm, &nl);
    if (r < 0) return r;
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    char dn[UX_NAMEMAX];                 /* path as given, for getsockname */
    int dl = 0;
    if (nl > 0 && nm[0] && alen > 2 && alen <= 2 + UX_NAMEMAX) {
        uint8_t raw[2 + UX_NAMEMAX];
        if (copy_from_user(raw, uaddr, alen) == 0)
            while (dl < (int)alen - 2 && dl < UX_NAMEMAX - 1 && raw[2 + dl]) { dn[dl] = (char)raw[2 + dl]; dl++; }
    }
    klock(&ux_lock);
    if (e->nlen) { kunlock(&ux_lock); return -EINVAL; }
    if (nl == 0) {                       /* autobind: abstract "\0XXXXX" */
        static const char hx[] = "0123456789abcdef";
        do {
            uint32_t c = ++ux_autobind_ctr;
            nm[0] = 0;
            for (int i = 0; i < 5; i++) nm[1 + i] = hx[(c >> (16 - 4 * i)) & 0xF];
            nl = 6;
        } while (ux_lookup(nm, nl));
    } else if (ux_lookup(nm, nl)) {
        kunlock(&ux_lock);
        return -EADDRINUSE;
    }
    memcpy(e->name, nm, (size_t)nl);
    e->nlen = nl;
    e->reg = true;
    e->dlen = (nl > 0 && nm[0]) ? dl : 0;
    if (e->dlen) memcpy(e->dname, dn, (size_t)dl);
    kunlock(&ux_lock);
    s->bound = true;
    return 0;
}

long ux_listen(kernel_socket_t *s, int backlog) {
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    if (e->type == UX_DGRAM) return -EOPNOTSUPP;
    klock(&ux_lock);
    if (!e->nlen || e->state == UXS_CONN) { kunlock(&ux_lock); return -EINVAL; }
    if (backlog < 1) backlog = 1;
    if (backlog > UX_BACKLOG) backlog = UX_BACKLOG;
    e->backlog = backlog;
    e->state = UXS_LISTEN;
    kunlock(&ux_lock);
    s->listening = true;
    return 0;
}

extern void serial_printf(const char *fmt, ...);
extern void fork_release_pending(process_t *p);
static int ux_sleep_dbg(process_t *p, ux_ep_t *e, int *spins, const char *who) {
    (void)e; (void)spins;
    (void)who;
    if (proc_kill_pending(p)) return -EINTR;
    fork_release_pending(p);          /* held fork newborns run only once the parent sleeps */
    if (global_scheduler.scheduler_running) thread_sleep(1);
    else __asm__ volatile("hlt");
    return 0;
}

static int ux_sleep(process_t *p) {
    if (proc_kill_pending(p)) return -EINTR;
    fork_release_pending(p);          /* held fork newborns run only once the parent sleeps */
    if (global_scheduler.scheduler_running) thread_sleep(1);
    else __asm__ volatile("hlt");
    return 0;
}

long ux_connect(process_t *p, int fd, kernel_socket_t *s, const void *uaddr, uint32_t alen) {
    char nm[UX_NAMEMAX];
    int nl = 0;
    if (alen >= 2) {
        uint16_t fam = 0;
        if (copy_from_user(&fam, uaddr, 2)) return -EFAULT;
        if (fam == 0 && s->type == UX_DGRAM) {           /* AF_UNSPEC: dissolve */
            klock(&ux_lock);
            UX(s)->peer = NULL; UX(s)->peer_gone = false; UX(s)->state = UXS_NEW;
            kunlock(&ux_lock);
            s->connected = false;
            return 0;
        }
    }
    long r = lx_unix_parse_addr(p, uaddr, alen, nm, &nl);
    if (r < 0) return r;
    if (nl == 0) return -EINVAL;
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    bool nb = (p->fds[fd].flags & O_NONBLOCK) != 0;
    for (;;) {
        klock(&ux_lock);
        ux_ep_t *t = ux_lookup(nm, nl);
        if (!t) { kunlock(&ux_lock); return nm[0] ? -ENOENT : -ECONNREFUSED; }
        if (t->type != e->type) { kunlock(&ux_lock); return -EPROTOTYPE; }
        if (e->type == UX_DGRAM) {
            e->peer = t;
            e->peer_gone = false;
            e->peer_pid = t->pid;
            e->state = UXS_CONN;
            kunlock(&ux_lock);
            s->connected = true;
            return 0;
        }
        if (e->state == UXS_CONN) { kunlock(&ux_lock); return -EISCONN; }
        if (e->state == UXS_LISTEN) { kunlock(&ux_lock); return -EINVAL; }
        if (t->state != UXS_LISTEN) { kunlock(&ux_lock); return -ECONNREFUSED; }
        if (t->nbl < t->backlog) {
            kunlock(&ux_lock);
            ux_ep_t *srv = ux_ep_new(e->type, NULL);
            if (!srv) return -ENOMEM;
            klock(&ux_lock);
            /* re-validate: listener may have gone while unlocked */
            if (ux_lookup(nm, nl) != t || t->state != UXS_LISTEN || t->nbl >= t->backlog ||
                e->state != UXS_NEW) {
                kunlock(&ux_lock);
                ux_destroy(srv);
                continue;
            }
            srv->pid = t->pid;
            srv->peer_pid = p->pid;
            srv->listener = t;
            memcpy(srv->name, t->name, (size_t)t->nlen);
            srv->nlen = t->nlen;
            srv->state = UXS_CONN;
            srv->peer = e;
            e->peer = srv;
            e->peer_pid = t->pid;
            e->state = UXS_CONN;
            t->bl[t->nbl++] = srv;
            kunlock(&ux_lock);
            s->connected = true;
            return 0;
        }
        kunlock(&ux_lock);
        if (nb) return -EAGAIN;
        r = ux_sleep(p);
        if (r) return r;
    }
}

static long ux_put_name(void *uaddr, uint32_t *ualen, const char *name, int nlen) {
    if (!uaddr || !ualen) return 0;
    uint32_t al;
    if (copy_from_user(&al, ualen, sizeof(al))) return -EFAULT;
    uint8_t o[2 + UX_NAMEMAX + 1];
    memset(o, 0, sizeof(o));
    o[0] = AF_UNIX_;
    uint32_t full = 2;
    if (nlen > 0) {
        memcpy(o + 2, name, (size_t)nlen);
        full = 2 + (uint32_t)nlen + (name[0] ? 1 : 0);   /* fs path: count NUL */
    }
    uint32_t n = al < full ? al : full;
    if (n && copy_to_user(uaddr, o, n)) return -EFAULT;
    return copy_to_user(ualen, &full, sizeof(full)) ? -EFAULT : 0;
}

long ux_accept(process_t *p, int fd, kernel_socket_t *s, void *uaddr, uint32_t *ualen, long flags) {
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    if (e->type == UX_DGRAM) return -EOPNOTSUPP;
    bool nb = (p->fds[fd].flags & O_NONBLOCK) != 0;
    for (;;) {
        klock(&ux_lock);
        if (e->state != UXS_LISTEN) { kunlock(&ux_lock); return -EINVAL; }
        if (e->nbl > 0) {
            ux_ep_t *srv = e->bl[0];
            for (int i = 1; i < e->nbl; i++) e->bl[i - 1] = e->bl[i];
            e->nbl--;
            srv->listener = NULL;
            char pn[UX_NAMEMAX];
            int pl = srv->peer ? ux_dn(srv->peer, pn) : 0;
            kunlock(&ux_lock);
            long nfd = ux_new_fd(p, srv, flags);
            if (nfd < 0) { ux_destroy(srv); return nfd; }
            ((kernel_socket_t *)p->fds[nfd].file_handle)->connected = true;
            if (uaddr && ualen) ux_put_name(uaddr, ualen, pn, pl);
            return nfd;
        }
        kunlock(&ux_lock);
        if (nb) return -EAGAIN;
        long r = ux_sleep(p);
        if (r) return r;
    }
}

long ux_name(kernel_socket_t *s, void *uaddr, uint32_t *ualen, bool peer) {
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    char nm[UX_NAMEMAX];
    int nl;
    klock(&ux_lock);
    ux_ep_t *src = e;
    if (peer) {
        if (!e->peer) { kunlock(&ux_lock); return -ENOTCONN; }
        src = e->peer;
    }
    nl = ux_dn(src, nm);
    kunlock(&ux_lock);
    if (!uaddr || !ualen) return -EFAULT;
    return ux_put_name(uaddr, ualen, nm, nl);
}

long ux_shutdown(kernel_socket_t *s, int how) {
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    if (how < 0 || how > 2) return -EINVAL;
    klock(&ux_lock);
    if (e->type != UX_DGRAM && e->state != UXS_CONN) { kunlock(&ux_lock); return -ENOTCONN; }
    if (how == 0 || how == 2) e->shut_rd = true;
    if (how == 1 || how == 2) {
        e->shut_wr = true;
        if (e->peer && e->type != UX_DGRAM) e->peer->rd_eof = true;
    }
    kunlock(&ux_lock);
    return 0;
}

long ux_getsockopt(kernel_socket_t *s, int level, int opt, void *uval, uint32_t *ulen) {
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    if (level != 1) return -ENOPROTOOPT;
    uint8_t buf[12];
    uint32_t n = 4;
    int v = 0;
    switch (opt) {
    case 3:  v = e->type; break;                       /* SO_TYPE */
    case 4:  v = 0; break;                             /* SO_ERROR */
    case 7: case 8: v = (int)UX_RXMAX; break;          /* SO_SNDBUF/RCVBUF */
    case 30: v = e->state == UXS_LISTEN; break;        /* SO_ACCEPTCONN */
    case 38: v = 0; break;                             /* SO_PROTOCOL */
    case 39: v = AF_UNIX_; break;                      /* SO_DOMAIN */
    case 17: {                                         /* SO_PEERCRED */
        uint32_t cred[3] = { e->peer_pid, 0, 0 };
        if (e->state != UXS_CONN) cred[0] = 0;
        memcpy(buf, cred, 12);
        n = 12;
        break;
    }
    default: v = 0; break;
    }
    if (n == 4) memcpy(buf, &v, 4);
    if (!uval || !ulen) return -EFAULT;
    uint32_t l;
    if (copy_from_user(&l, ulen, sizeof(l))) return -EFAULT;
    if (l > n) l = n;
    if (copy_to_user(uval, buf, l)) return -EFAULT;
    return copy_to_user(ulen, &l, sizeof(l)) ? -EFAULT : 0;
}

/* ── poll ──────────────────────────────────────────────────────────────── */
int ux_poll(kernel_socket_t *s, int events) {
    ux_ep_t *e = UX(s);
    if (!e) return 0x8;
    int rv = 0;
    klock(&ux_lock);
    if (e->state == UXS_LISTEN) {
        if (e->nbl > 0) rv |= 0x1 | 0x40;
    } else if (e->type == UX_DGRAM) {
        if (e->rx_count > 0) rv |= 0x1 | 0x40;
        ux_ep_t *t = e->peer;
        if (!t || t->rx_bytes < UX_RXMAX) rv |= 0x4 | 0x100;
        if (e->shut_rd) rv |= 0x1 | 0x40;
    } else if (e->state == UXS_CONN) {
        if (e->rx_count > 0 || e->rd_eof || e->shut_rd) rv |= 0x1 | 0x40;
        if (e->rd_eof) rv |= 0x2000;                      /* POLLRDHUP */
        if (e->peer && !e->shut_wr && e->peer->rx_bytes < UX_RXMAX && !e->peer->shut_rd)
            rv |= 0x4 | 0x100;
        if (e->peer_gone || (e->shut_wr && e->rd_eof)) rv |= 0x10;   /* POLLHUP */
        if (e->peer_gone && !e->shut_wr) rv |= 0x4 | 0x100;  /* write -> EPIPE */
    } else {
        rv |= 0x4 | 0x10;                                  /* unconnected stream */
    }
    kunlock(&ux_lock);
    return rv & (events | 0x8 | 0x10);
}

/* ── data path ─────────────────────────────────────────────────────────── */
typedef struct { uint64_t base, len; } ux_iov_t;

static long ux_iov_total(const ux_iov_t *iov, int n) {
    uint64_t t = 0;
    for (int i = 0; i < n; i++) t += iov[i].len;
    return t > 0x7fffffff ? -EINVAL : (long)t;
}

/* copy up to max bytes from the user iovec (starting at byte `skip`) */
static long ux_gather(const ux_iov_t *iov, int n, uint64_t skip, uint8_t *k, uint32_t max) {
    uint32_t done = 0;
    for (int i = 0; i < n && done < max; i++) {
        uint64_t l = iov[i].len, b = iov[i].base;
        if (skip >= l) { skip -= l; continue; }
        b += skip; l -= skip; skip = 0;
        if (l > max - done) l = max - done;
        if (l && copy_from_user(k + done, (const void *)b, (size_t)l)) return -EFAULT;
        done += (uint32_t)l;
    }
    return done;
}

static long ux_scatter(const ux_iov_t *iov, int n, const uint8_t *k, uint32_t len) {
    uint32_t done = 0;
    for (int i = 0; i < n && done < len; i++) {
        uint64_t l = iov[i].len;
        if (l > len - done) l = len - done;
        if (l && copy_to_user((void *)iov[i].base, k + done, (size_t)l)) return -EFAULT;
        done += (uint32_t)l;
    }
    return done;
}

/* Parse SCM_RIGHTS out of a user control buffer; takes fd refs. */
static long ux_parse_cmsg(process_t *p, const void *uctl, uint64_t clen,
                          file_descriptor_t **out, int *nout) {
    *out = NULL; *nout = 0;
    if (!uctl || clen < 16) return 0;
    if (clen > 4096) return -EINVAL;
    uint8_t *c = (uint8_t *)kalloc((size_t)clen);
    if (!c) return -ENOMEM;
    if (copy_from_user(c, uctl, (size_t)clen)) { kfree(c); return -EFAULT; }
    int fds[UX_MAXFDS], nf = 0;
    uint64_t off = 0;
    long r = 0;
    while (off + 16 <= clen) {
        uint64_t len; int lvl, typ;
        memcpy(&len, c + off, 8); memcpy(&lvl, c + off + 8, 4); memcpy(&typ, c + off + 12, 4);
        if (len < 16 || off + len > clen) { r = -EINVAL; break; }
        if (lvl == 1 && typ == 1) {                     /* SOL_SOCKET / SCM_RIGHTS */
            int cnt = (int)((len - 16) / 4);
            for (int i = 0; i < cnt; i++) {
                if (nf >= UX_MAXFDS) { r = -EINVAL; break; }
                memcpy(&fds[nf++], c + off + 16 + 4 * i, 4);
            }
            if (r) break;
        }
        off += (len + 7) & ~7ull;
    }
    kfree(c);
    if (r || nf == 0) return r;
    file_descriptor_t *f = (file_descriptor_t *)kalloc(sizeof(file_descriptor_t) * (size_t)nf);
    if (!f) return -ENOMEM;
    for (int i = 0; i < nf; i++) {
        long e = fd_flight_get(p, fds[i], &f[i]);
        if (e) {
            for (int j = 0; j < i; j++) fd_flight_release(&f[j]);
            kfree(f);
            return e;
        }
    }
    *out = f; *nout = nf;
    return 0;
}

long ux_sendmsg(process_t *p, int fd, kernel_socket_t *s, const ux_iov_t *iov, int niov,
                const void *uname, uint32_t namelen, const void *uctl, uint64_t ctllen, int flags) {
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    long total = ux_iov_total(iov, niov);
    if (total < 0) return total;
    bool nb = (p->fds[fd].flags & O_NONBLOCK) || (flags & MSG_DONTWAIT_);
    bool msgmode = e->type != UX_STREAM;
    if (msgmode && (uint64_t)total > UX_MSGMAX) return -EMSGSIZE;
    if (!msgmode && total == 0) {
        if (e->shut_wr) return -EPIPE;
        return e->state == UXS_CONN ? 0 : -ENOTCONN;
    }

    char to[UX_NAMEMAX];
    int tol = 0;
    if (uname && namelen && e->type == UX_DGRAM) {
        long r = lx_unix_parse_addr(p, uname, namelen, to, &tol);
        if (r < 0) return r;
    }
    file_descriptor_t *fl = NULL;
    int nfl = 0;
    long r = ux_parse_cmsg(p, uctl, ctllen, &fl, &nfl);
    if (r < 0) return r;

    uint64_t sent = 0;
    long result = 0;
    int spins = 0;
    for (;;) {
        uint32_t want = (uint32_t)((uint64_t)total - sent);
        if (want > UX_MSGMAX) want = UX_MSGMAX;
        ux_msg_t *m = (ux_msg_t *)kalloc(sizeof(ux_msg_t) + (want ? want : 1));
        if (!m) { result = -ENOMEM; break; }
        memset(m, 0, sizeof(*m));
        long g = ux_gather(iov, niov, sent, m->data, want);
        if (g < 0) { kfree(m); result = -EFAULT; break; }
        m->len = (uint32_t)g;

        /* wait for room */
        long err = 0;
        for (;;) {
            klock(&ux_lock);
            ux_ep_t *t;
            if (e->type == UX_DGRAM) {
                if (tol) {
                    t = ux_lookup(to, tol);
                    if (!t) err = to[0] ? -ENOENT : -ECONNREFUSED;
                    else if (t->type != UX_DGRAM) err = -EPROTOTYPE;
                } else {
                    t = e->peer;
                    if (!t) err = e->peer_gone ? -ECONNREFUSED : -ENOTCONN;
                }
                if (!err && e->shut_wr) err = -EPIPE;
            } else {
                t = e->peer;
                if (e->shut_wr || (e->state == UXS_CONN && (!t || t->shut_rd))) err = -EPIPE;
                else if (e->state != UXS_CONN || !t) err = -ENOTCONN;
            }
            if (err) { kunlock(&ux_lock); break; }
            uint32_t room = t->rx_bytes >= UX_RXMAX ? 0 : UX_RXMAX - t->rx_bytes;
            bool fits = msgmode ? (t->rx_count < UX_MAXQ && (room >= m->len || t->rx_bytes == 0))
                                : (room > 0 && t->rx_count < UX_MAXQ);
            if (fits) {
                if (!msgmode && m->len > room) m->len = room;
                if (fl && sent == 0) { m->fds = fl; m->nfds = nfl; fl = NULL; }
                m->fromlen = ux_dn(e, m->from);
                if (t->rxt) t->rxt->next = m; else t->rxh = m;
                t->rxt = m;
                t->rx_bytes += m->len;
                t->rx_count++;
                kunlock(&ux_lock);
                break;
            }
            kunlock(&ux_lock);
            if (nb) { err = -EAGAIN; break; }
            err = ux_sleep_dbg(p, e, &spins, "send");
            if (err) break;
        }
        if (err) {
            kfree(m);
            if (err == -EPIPE && !(flags & MSG_NOSIGNAL_) && sent == 0) proc_raise_sigpipe(p);
            result = sent ? (long)sent : err;
            break;
        }
        sent += m->len;
        if (msgmode || sent >= (uint64_t)total) { result = (long)sent; break; }
        if (nb) {
            /* non-blocking stream: return the partial count unless room remains */
            klock(&ux_lock);
            bool more = e->peer && e->peer->rx_bytes < UX_RXMAX;
            kunlock(&ux_lock);
            if (!more) { result = (long)sent; break; }
        }
    }
    if (fl) {
        for (int i = 0; i < nfl; i++) fd_flight_release(&fl[i]);
        kfree(fl);
    }
    return result;
}

/* recvmsg: ctl/ctllen user control buffer; *octl = bytes of control used,
 * *oflags = msg_flags. uname/unamelen: user address out (unamelen user ptr). */
long ux_recvmsg(process_t *p, int fd, kernel_socket_t *s, const ux_iov_t *iov, int niov,
                void *uname, uint32_t *unamelen, void *uctl, uint64_t ctllen,
                uint64_t *octl, int *oflags, int flags) {
    ux_ep_t *e = UX(s);
    if (!e) return -EBADF;
    *octl = 0; *oflags = 0;
    long total = ux_iov_total(iov, niov);
    if (total < 0) return total;
    bool nb = (p->fds[fd].flags & O_NONBLOCK) || (flags & MSG_DONTWAIT_);
    bool peek = (flags & MSG_PEEK_) != 0;
    bool msgmode = e->type != UX_STREAM;
    /* one message at most per call; stream reads may be short */
    uint32_t cap = (uint32_t)((uint64_t)total > UX_MSGMAX ? UX_MSGMAX : (uint64_t)total);
    uint8_t *k = (uint8_t *)kalloc(cap ? cap : 1);
    if (!k) { serial_printf("ux: recv kalloc(%u) failed\n", cap); return -ENOMEM; }
    int spins = 0;

    uint32_t got = 0, mlen = 0;
    file_descriptor_t *fl = NULL;
    int nfl = 0;
    char from[UX_NAMEMAX];
    int froml = 0;
    ux_msg_t *dead = NULL;
    long r = 0;
    for (;;) {
        klock(&ux_lock);
        if (e->type != UX_DGRAM && e->state != UXS_CONN) {
            kunlock(&ux_lock); r = -ENOTCONN; break;
        }
        if (e->rxh) {
            if (msgmode) {
                ux_msg_t *m = e->rxh;
                mlen = m->len;
                got = m->len < cap ? m->len : cap;
                memcpy(k, m->data, got);
                froml = m->fromlen;
                if (froml) memcpy(from, m->from, (size_t)froml);
                if (!peek) {
                    e->rxh = m->next;
                    if (!e->rxh) e->rxt = NULL;
                    e->rx_bytes -= m->len;
                    e->rx_count--;
                    fl = m->fds; nfl = m->nfds; m->fds = NULL;
                    m->next = NULL;          /* else the free loop walks into the live queue */
                    dead = m;
                }
            } else {
                ux_msg_t *m = e->rxh;
                while (m && got < cap) {
                    if (m->fds && got > 0) break;        /* fds start a new read */
                    if (m->fds && !peek) { fl = m->fds; nfl = m->nfds; m->fds = NULL; }
                    uint32_t avail = m->len - m->off;
                    uint32_t n = avail < cap - got ? avail : cap - got;
                    memcpy(k + got, m->data + m->off, n);
                    got += n;
                    if (peek) { m = m->next; continue; }
                    m->off += n;
                    e->rx_bytes -= n;
                    if (m->off >= m->len) {
                        ux_msg_t *nx = m->next;
                        e->rxh = nx;
                        if (!nx) e->rxt = NULL;
                        e->rx_count--;
                        m->next = dead; dead = m;
                        m = nx;
                    } else break;
                }
                froml = e->peer ? ux_dn(e->peer, from) : 0;
                if (got == 0 && cap == 0) { /* zero-length read with data pending */ }
            }
            kunlock(&ux_lock);
            r = msgmode ? (long)got : (long)got;
            break;
        }
        if (e->shut_rd || (e->type != UX_DGRAM && e->rd_eof)) {
            kunlock(&ux_lock); r = 0; break;
        }
        kunlock(&ux_lock);
        if (nb) { r = -EAGAIN; break; }
        r = ux_sleep_dbg(p, e, &spins, "recv");
        if (r) break;
    }
    while (dead) { ux_msg_t *n = dead->next; ux_msg_free(dead); dead = n; }

    if (r >= 0) {
        long w = ux_scatter(iov, niov, k, got);
        if (w < 0) r = w;
        if (msgmode && mlen > got) {
            *oflags |= MSG_TRUNC_;
            if (flags & MSG_TRUNC_) r = (long)mlen;
        }
        if (uname && unamelen && r >= 0) ux_put_name(uname, unamelen, from, froml);
    }
    if (fl) {
        /* deliver as one SCM_RIGHTS cmsg */
        uint64_t need = 16 + 4 * (uint64_t)nfl;
        int nput = nfl;
        if (!uctl || ctllen < need) {
            *oflags |= MSG_CTRUNC_;
            nput = (uctl && ctllen >= 20) ? (int)((ctllen - 16) / 4) : 0;
        }
        if (r < 0) nput = 0;
        if (nput > 0) {
            uint8_t hdr[16];
            uint64_t len = 16 + 4 * (uint64_t)nput;
            int lvl = 1, typ = 1;
            memcpy(hdr, &len, 8); memcpy(hdr + 8, &lvl, 4); memcpy(hdr + 12, &typ, 4);
            int fdsout[UX_MAXFDS];
            int ok = 0;
            for (int i = 0; i < nput; i++) {
                int nf = fd_flight_install(p, &fl[i], (flags & MSG_CMSG_CLOEXEC_) != 0);
                if (nf < 0) { fd_flight_release(&fl[i]); *oflags |= MSG_CTRUNC_; continue; }
                fdsout[ok++] = nf;
            }
            len = 16 + 4 * (uint64_t)ok;
            memcpy(hdr, &len, 8);
            if (ok > 0 && copy_to_user(uctl, hdr, 16) == 0 &&
                copy_to_user((uint8_t *)uctl + 16, fdsout, 4 * (size_t)ok) == 0)
                *octl = ((len + 7) & ~7ull) <= ctllen ? ((len + 7) & ~7ull) : len;
        }
        for (int i = nput; i < nfl; i++) fd_flight_release(&fl[i]);
        kfree(fl);
    }
    kfree(k);
    return r;
}
