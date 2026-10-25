/*
 * lx_extra.c - small Linux-ABI helpers: advisory flock() table, named FIFOs
 * (mkfifo/mknod S_IFIFO) and per-process nice values.
 *
 * flock: whole-file advisory locks keyed by (path, open file description).
 *   The owner key is the fd's shared ref_count cell, so dup()/fork() share the
 *   lock like Linux; the lock goes away when the last reference is closed.
 * FIFO: an in-kernel registry path -> pipe_t. The filesystem only holds an
 *   empty placeholder file (FAT32/exFAT/ext have no FIFO inodes here), so the
 *   FIFO identity is lost at reboot. open() pairs readers and writers like
 *   Linux (blocking until the other side opens unless O_NONBLOCK).
 */
#include "syscall_internal.h"
#include "mem.h"
#include "string.h"
#include "thread.h"
#include "pipe.h"
#include "atomic.h"
#include "vfs.h"
#include "uaccess.h"
#include "../include/indevos_errno.h"
#ifndef ENOLCK
#define ENOLCK 37
#endif

extern bool proc_kill_pending(process_t *p);
extern int allocate_fd(process_t *process);
extern void free_fd(process_t *process, int fd);
extern uint32_t *fd_refcount_create(void);

extern void fork_release_pending(process_t *p);
static void lxe_sleep(process_t *p) {
    fork_release_pending(p);          /* held fork newborns run only once the parent sleeps */
    if (global_scheduler.scheduler_running) thread_sleep(1);
    else __asm__ volatile("hlt");
}

/* ── flock ──────────────────────────────────────────────────────────────── */
#define FLK_MAX 128
typedef struct { bool used; void *owner; int type; char path[VFS_MAX_PATH]; } flk_t;
static flk_t flk[FLK_MAX];
static spinlock_t flk_lock;
static volatile int flk_n;

/* op: LOCK_SH 1, LOCK_EX 2, LOCK_NB 4, LOCK_UN 8 */
long flk_op(process_t *p, void *owner, const char *path, int op) {
    int nb = op & 4;
    op &= ~4;
    if (op != 1 && op != 2 && op != 8) return -EINVAL;
    for (;;) {
        klock(&flk_lock);
        int mine = -1, conflict = 0, freei = -1;
        for (int i = 0; i < FLK_MAX; i++) {
            if (!flk[i].used) { if (freei < 0) freei = i; continue; }
            if (strcmp(flk[i].path, path) != 0) continue;
            if (flk[i].owner == owner) mine = i;
            else if (op == 2 || flk[i].type == 2) conflict = 1;
        }
        if (op == 8) {
            if (mine >= 0) { flk[mine].used = false; flk_n--; }
            kunlock(&flk_lock);
            return 0;
        }
        if (!conflict) {
            if (mine >= 0) flk[mine].type = op;
            else if (freei < 0) { kunlock(&flk_lock); return -ENOLCK; }
            else {
                flk[freei].used = true;
                flk[freei].owner = owner;
                flk[freei].type = op;
                strncpy(flk[freei].path, path, VFS_MAX_PATH - 1);
                flk[freei].path[VFS_MAX_PATH - 1] = 0;
                flk_n++;
            }
            kunlock(&flk_lock);
            return 0;
        }
        /* Linux drops an existing lock before blocking on a conversion */
        if (mine >= 0 && !nb) { flk[mine].used = false; flk_n--; }
        kunlock(&flk_lock);
        if (nb) return -EAGAIN;
        if (proc_kill_pending(p)) return -EINTR;
        lxe_sleep(p);
    }
}

/* last close of an open file description */
void flk_release(void *owner) {
    if (!flk_n || !owner) return;
    klock(&flk_lock);
    for (int i = 0; i < FLK_MAX; i++)
        if (flk[i].used && flk[i].owner == owner) { flk[i].used = false; flk_n--; }
    kunlock(&flk_lock);
}

/* ── named FIFOs ────────────────────────────────────────────────────────── */
#define FIFO_MAX 32
typedef struct { bool used; pipe_t *pp; uint32_t rgen, wgen; char path[VFS_MAX_PATH]; } fifo_t;
static fifo_t fifos[FIFO_MAX];
static spinlock_t fifo_lock;
static volatile int fifo_n;

static int fifo_find(const char *path) {
    for (int i = 0; i < FIFO_MAX; i++)
        if (fifos[i].used && strcmp(fifos[i].path, path) == 0) return i;
    return -1;
}

int fifo_reg(const char *path) {
    klock(&fifo_lock);
    if (fifo_find(path) >= 0) { kunlock(&fifo_lock); return -EEXIST; }
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifos[i].used) continue;
        memset(&fifos[i], 0, sizeof(fifos[i]));
        fifos[i].used = true;
        strncpy(fifos[i].path, path, VFS_MAX_PATH - 1);
        fifo_n++;
        kunlock(&fifo_lock);
        return 0;
    }
    kunlock(&fifo_lock);
    return -ENOSPC;
}

bool fifo_is(const char *path) {
    if (!fifo_n) return false;
    klock(&fifo_lock);
    bool r = fifo_find(path) >= 0;
    kunlock(&fifo_lock);
    return r;
}

void fifo_unreg(const char *path) {
    if (!fifo_n) return;
    klock(&fifo_lock);
    int i = fifo_find(path);
    if (i >= 0) { fifos[i].used = false; fifos[i].pp = NULL; fifo_n--; }
    kunlock(&fifo_lock);
}

void fifo_rename(const char *from, const char *to) {
    if (!fifo_n) return;
    klock(&fifo_lock);
    int j = fifo_find(to);
    if (j >= 0) { fifos[j].used = false; fifos[j].pp = NULL; fifo_n--; }
    int i = fifo_find(from);
    if (i >= 0) { strncpy(fifos[i].path, to, VFS_MAX_PATH - 1); fifos[i].path[VFS_MAX_PATH - 1] = 0; }
    kunlock(&fifo_lock);
}

/* pipe.c: called at the start of pipe_destroy (before the slot is reusable) */
void fifo_pipe_gone(pipe_t *pp) {
    if (!fifo_n) return;
    klock(&fifo_lock);
    for (int i = 0; i < FIFO_MAX; i++)
        if (fifos[i].used && fifos[i].pp == pp) fifos[i].pp = NULL;
    kunlock(&fifo_lock);
}

static int fifo_install(process_t *p, pipe_t *pp, bool rd, bool nonblock, bool cloexec) {
    int fd = allocate_fd(p);
    if (fd < 0) return -EMFILE;
    uint32_t *rc = fd_refcount_create();
    if (!rc) { free_fd(p, fd); return -ENOMEM; }
    memset(&p->fds[fd], 0, sizeof(file_descriptor_t));
    p->fds[fd].in_use = true;
    p->fds[fd].fd_type = rd ? FD_TYPE_PIPE_READ : FD_TYPE_PIPE_WRITE;
    p->fds[fd].file_handle = pp;
    p->fds[fd].ref_count = rc;
    p->fds[fd].flags = nonblock ? O_NONBLOCK : 0;
    p->fds[fd].cloexec = cloexec;
    return fd;
}

/* acc: 0 read, 1 write, 2 rdwr (served as a non-blocking read end) */
long fifo_open(process_t *p, const char *path, int acc, bool nonblock, bool cloexec) {
    pipe_t *fresh = NULL;
    for (;;) {
        klock(&fifo_lock);
        int i = fifo_find(path);
        if (i < 0) { kunlock(&fifo_lock); if (fresh) pipe_destroy(fresh); return -ENOENT; }
        fifo_t *f = &fifos[i];
        pipe_t *pp = f->pp;
        if (pp && (!pp->active || pp->readers + pp->writers <= 0)) pp = f->pp = NULL;
        if (!pp) {
            if (!fresh) {
                kunlock(&fifo_lock);
                fresh = pipe_create();
                if (!fresh) return -ENFILE;
                continue;
            }
            fresh->readers = 0;
            fresh->writers = 0;
            pp = f->pp = fresh;
            fresh = NULL;
        }
        if (acc == 1 && nonblock && pp->readers == 0) {
            bool idle = pp->writers == 0;
            if (idle) f->pp = NULL;
            kunlock(&fifo_lock);
            if (idle) { pp->readers = pp->writers = 0; pipe_destroy(pp); }
            if (fresh) pipe_destroy(fresh);
            return -ENXIO;
        }
        bool rd = acc != 1;
        if (rd) { pp->readers++; f->rgen++; } else { pp->writers++; f->wgen++; }
        uint32_t gen = rd ? f->wgen : f->rgen;
        kunlock(&fifo_lock);
        if (fresh) pipe_destroy(fresh);

        int fd = fifo_install(p, pp, rd, nonblock, cloexec);
        if (fd < 0) {
            if (rd) pipe_close_read(pp); else pipe_close_write(pp);
            return fd;
        }
        if (nonblock || acc == 2) return fd;
        /* wait for the other side to open (once) */
        for (;;) {
            klock(&fifo_lock);
            bool ok = rd ? (pp->writers > 0) : (pp->readers > 0);
            int j = fifo_find(path);
            if (!ok && j >= 0 && fifos[j].pp == pp)
                ok = rd ? fifos[j].wgen != gen : fifos[j].rgen != gen;
            if (j < 0 || fifos[j].pp != pp) ok = true;   /* unlinked: stop waiting */
            kunlock(&fifo_lock);
            if (ok) return fd;
            if (proc_kill_pending(p)) { sys_close(fd); return -EINTR; }
            lxe_sleep(p);
        }
    }
}

/* ── nice values (getpriority/setpriority) ─────────────────────────────── */
#define NICE_MAX 128
static struct { uint32_t pid; int8_t nice; } nice_tab[NICE_MAX];
static spinlock_t nice_lock;

int lx_nice_get(uint32_t pid) {
    int v = 0;
    klock(&nice_lock);
    for (int i = 0; i < NICE_MAX; i++) if (nice_tab[i].pid == pid) { v = nice_tab[i].nice; break; }
    kunlock(&nice_lock);
    return v;
}

void lx_nice_set(uint32_t pid, int v) {
    klock(&nice_lock);
    int slot = -1;
    for (int i = 0; i < NICE_MAX; i++) if (nice_tab[i].pid == pid) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < NICE_MAX; i++) if (nice_tab[i].pid == 0) { slot = i; break; }
    if (slot < 0) slot = (int)(pid % NICE_MAX);
    nice_tab[slot].pid = v ? pid : 0;
    nice_tab[slot].nice = (int8_t)v;
    kunlock(&nice_lock);
}

/* ── memfd_create: hidden tmpfs file, unlinked at the last close ─────────── */
#define MEMFD_MAX 64
static struct { void *owner; char path[48]; } memfds[MEMFD_MAX];
static spinlock_t memfd_lock;
static volatile int memfd_n;
static uint32_t memfd_seq;


static char *lxe_utoa(char *o, uint32_t v) {
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) *o++ = t[--k];
    return o;
}

long memfd_create_k(process_t *p, const char *uname, unsigned flags) {
    if (flags & ~0x1bu) return -EINVAL;   /* CLOEXEC|ALLOW_SEALING|NOEXEC_SEAL|EXEC; no HUGETLB */
    if (!uname) return -EFAULT;
    char nm[250];
    int i = 0;
    for (; i < 250; i++) {
        if (copy_from_user(&nm[i], uname + i, 1)) return -EFAULT;
        if (!nm[i]) break;
    }
    if (i >= 250) return -EINVAL;
    char path[48];
    klock(&memfd_lock);
    uint32_t seq = ++memfd_seq;
    kunlock(&memfd_lock);
    char *o = path;
    const char *pre = "/tmp/.memfd.";
    while (*pre) *o++ = *pre++;
    o = lxe_utoa(o, (uint32_t)p->pid);
    *o++ = '.';
    o = lxe_utoa(o, seq);
    *o = 0;
    long fd = sys_open_kpath(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return fd;
    if (!p->fds[fd].ref_count) return fd;           /* cannot track: left as a plain file */
    klock(&memfd_lock);
    int slot = -1;
    for (int k = 0; k < MEMFD_MAX; k++) if (!memfds[k].owner) { slot = k; break; }
    if (slot >= 0) {
        memfds[slot].owner = p->fds[fd].ref_count;
        memcpy(memfds[slot].path, path, sizeof(path));
        memfd_n++;
    }
    kunlock(&memfd_lock);
    if (slot < 0) { sys_close((int)fd); vfs_unlink(path); return -EMFILE; }
    if (flags & 1) p->fds[fd].cloexec = true;
    return fd;
}

void memfd_release(void *owner) {
    if (!memfd_n || !owner) return;
    char path[48];
    path[0] = 0;
    klock(&memfd_lock);
    for (int k = 0; k < MEMFD_MAX; k++)
        if (memfds[k].owner == owner) {
            memcpy(path, memfds[k].path, sizeof(path));
            memfds[k].owner = NULL;
            memfd_n--;
            break;
        }
    kunlock(&memfd_lock);
    if (path[0]) vfs_unlink(path);
}
