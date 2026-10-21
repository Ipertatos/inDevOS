/* Read-only ext2/3/4 driver.  See ext4ro.h. */
#include "ext4ro.h"
#include "storage.h"
#include "mem.h"
#include "string.h"
#include "serial.h"

extern int printf(const char *fmt, ...);

/* incompat */
#define I_FILETYPE   0x0002
#define I_RECOVER    0x0004
#define I_META_BG    0x0010
#define I_EXTENTS    0x0040
#define I_64BIT      0x0080
#define I_MMP        0x0100
#define I_FLEX_BG    0x0200
#define I_EA_INODE   0x0400
#define I_CSUM_SEED  0x2000
#define I_LARGEDIR   0x4000
#define I_INLINE     0x8000
#define I_SUPPORTED  (I_FILETYPE | I_RECOVER | I_META_BG | I_EXTENTS | I_64BIT | I_MMP | I_FLEX_BG | \
                      I_EA_INODE | I_CSUM_SEED | I_LARGEDIR | I_INLINE)
#define FL_EXTENTS   0x00080000u
#define FL_INLINE    0x10000000u
#define S_IFMT_  0xF000
#define S_IFDIR_ 0x4000
#define S_IFREG_ 0x8000
#define S_IFLNK_ 0xA000

struct e4_vol {
    uint8_t  dev;
    uint32_t part;
    uint32_t bs, spb;               /* block size, sectors per block */
    uint32_t ipg, bpg, isz, dsz, groups, first_data;
    uint32_t incompat, ro_compat;
    uint64_t blocks;
    char     label[17];
    char     mpath[64];
    uint8_t *gdt;                   /* all group descriptors */
};

typedef struct {
    uint16_t mode;
    uint32_t uid, gid, flags, links;
    uint64_t size;
    uint32_t mtime;
    uint8_t  iblock[60];
    uint8_t  raw[1024];             /* whole on-disk inode (inline data xattr) */
    uint32_t rawlen;
} e4_inode_t;

#define E4_MAX_VOLS 8
static e4_vol_t *e4_vols[E4_MAX_VOLS];

static int e4_read(e4_vol_t *v, uint64_t blk, void *buf) {
    return storage_read_sectors(v->dev, v->part + blk * v->spb, v->spb, buf);
}

bool ext4ro_probe(uint8_t dev, uint32_t lba) {
    static uint8_t sb[1024];
    if (storage_read_sectors(dev, lba + 2, 2, sb) != 0) return false;
    return *(uint16_t *)(sb + 56) == 0xEF53;
}

static uint64_t e4_itable(e4_vol_t *v, uint32_t g) {
    const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    uint64_t t = *(uint32_t *)(d + 8);
    if (v->dsz >= 64) t |= (uint64_t)*(uint32_t *)(d + 0x28) << 32;
    return t;
}

static int e4_iget(e4_vol_t *v, uint32_t ino, e4_inode_t *out) {
    if (ino == 0) return -1;
    uint32_t g = (ino - 1) / v->ipg, idx = (ino - 1) % v->ipg;
    if (g >= v->groups) return -1;
    uint64_t off = (uint64_t)idx * v->isz;
    uint64_t blk = e4_itable(v, g) + off / v->bs;
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -1;
    if (e4_read(v, blk, b) != 0) { kfree(b); return -1; }
    const uint8_t *r = b + off % v->bs;
    out->rawlen = v->isz > sizeof(out->raw) ? sizeof(out->raw) : v->isz;
    memcpy(out->raw, r, out->rawlen);
    kfree(b);
    r = out->raw;
    out->mode = *(uint16_t *)(r + 0);
    out->uid = *(uint16_t *)(r + 2) | ((uint32_t)*(uint16_t *)(r + 120) << 16);
    out->gid = *(uint16_t *)(r + 24) | ((uint32_t)*(uint16_t *)(r + 122) << 16);
    out->size = *(uint32_t *)(r + 4) | ((uint64_t)*(uint32_t *)(r + 108) << 32);
    out->mtime = *(uint32_t *)(r + 16);
    out->links = *(uint16_t *)(r + 26);
    out->flags = *(uint32_t *)(r + 32);
    memcpy(out->iblock, r + 40, 60);
    return 0;
}

/* logical -> physical block (0 = hole / unwritten). */
static uint64_t e4_bmap(e4_vol_t *v, const e4_inode_t *in, uint32_t lblk) {
    if (in->flags & FL_EXTENTS) {
        const uint8_t *node = in->iblock;
        uint8_t *buf = NULL;
        uint64_t res = 0;
        for (int depth_guard = 0; depth_guard < 6; depth_guard++) {
            if (*(uint16_t *)node != 0xF30A) break;
            uint16_t n = *(uint16_t *)(node + 2), depth = *(uint16_t *)(node + 6);
            const uint8_t *e = node + 12;
            if (depth == 0) {
                for (int i = 0; i < n; i++, e += 12) {
                    uint32_t lb = *(uint32_t *)e; uint16_t len = *(uint16_t *)(e + 4);
                    bool unwritten = len > 32768; if (unwritten) len -= 32768;
                    if (lblk >= lb && lblk < lb + len) {
                        if (!unwritten) {
                            uint64_t start = *(uint32_t *)(e + 8) | ((uint64_t)*(uint16_t *)(e + 6) << 32);
                            res = start + (lblk - lb);
                        }
                        break;
                    }
                }
                break;
            }
            int pick = -1;
            for (int i = 0; i < n; i++) if (*(uint32_t *)(e + 12 * i) <= lblk) pick = i;
            if (pick < 0) break;
            const uint8_t *ix = e + 12 * pick;
            uint64_t leaf = *(uint32_t *)(ix + 4) | ((uint64_t)*(uint16_t *)(ix + 8) << 32);
            if (!buf && !(buf = (uint8_t *)kalloc(v->bs))) break;
            if (e4_read(v, leaf, buf) != 0) break;
            node = buf;
        }
        if (buf) kfree(buf);
        return res;
    }
    /* legacy block map */
    const uint32_t *ib = (const uint32_t *)in->iblock;
    uint32_t per = v->bs / 4;
    if (lblk < 12) return ib[lblk];
    lblk -= 12;
    uint32_t lvl, root;
    if (lblk < per) { lvl = 1; root = ib[12]; }
    else if ((lblk -= per) < per * per) { lvl = 2; root = ib[13]; }
    else { lblk -= per * per; lvl = 3; root = ib[14]; }
    uint32_t *buf = (uint32_t *)kalloc(v->bs);
    if (!buf) return 0;
    uint32_t cur = root;
    for (uint32_t l = lvl; l > 0 && cur; l--) {
        if (e4_read(v, cur, buf) != 0) { cur = 0; break; }
        uint32_t div = 1;
        for (uint32_t k = 1; k < l; k++) div *= per;
        cur = buf[(lblk / div) % per];
    }
    kfree(buf);
    return cur;
}

/* Inline data: first 60 bytes in i_block, the rest in the "system.data"
 * in-inode xattr.  Copies up to @cap bytes of the inline payload. */
static uint32_t e4_inline(e4_vol_t *v, const e4_inode_t *in, uint8_t *out, uint32_t cap) {
    uint32_t n = 0;
    for (; n < 60 && n < cap; n++) out[n] = in->iblock[n];
    uint16_t extra = in->rawlen > 130 ? *(uint16_t *)(in->raw + 128) : 0;
    uint32_t xo = 128 + extra;
    if (xo + 4 > in->rawlen || *(uint32_t *)(in->raw + xo) != 0xEA020000u) return n;
    const uint8_t *base = in->raw + xo + 4, *e = base;
    while (e + 16 <= in->raw + in->rawlen && *(uint32_t *)e != 0) {
        uint8_t nl = e[0], idx = e[1];
        uint16_t voff = *(uint16_t *)(e + 2); uint32_t vsz = *(uint32_t *)(e + 8);
        if (idx == 7 && nl == 4 && memcmp(e + 16, "data", 4) == 0) {
            for (uint32_t i = 0; i < vsz && n < cap && base + voff + i < in->raw + in->rawlen; i++)
                out[n++] = base[voff + i];
            break;
        }
        e += (16 + nl + 3) & ~3u;
    }
    return n;
}

/* Read file bytes [off, off+len). Holes/unwritten read as zeros. */
static int e4_pread(e4_vol_t *v, const e4_inode_t *in, uint64_t off, uint8_t *dst, uint32_t len) {
    if (off >= in->size) return 0;
    if (len > in->size - off) len = (uint32_t)(in->size - off);
    if (in->flags & FL_INLINE) {
        uint8_t *tmp = (uint8_t *)kalloc(1024);
        if (!tmp) return -1;
        uint32_t n = e4_inline(v, in, tmp, 1024);
        uint32_t c = 0;
        for (; c < len && off + c < n; c++) dst[c] = tmp[off + c];
        for (; c < len; c++) dst[c] = 0;
        kfree(tmp);
        return (int)len;
    }
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -1;
    uint32_t done = 0;
    while (done < len) {
        uint64_t p = off + done;
        uint32_t lb = (uint32_t)(p / v->bs), in_off = (uint32_t)(p % v->bs);
        uint32_t n = v->bs - in_off; if (n > len - done) n = len - done;
        uint64_t pb = e4_bmap(v, in, lb);
        if (!pb) memset(dst + done, 0, n);
        else if (in_off == 0 && n == v->bs) {
            if (e4_read(v, pb, dst + done) != 0) { kfree(b); return done ? (int)done : -1; }
        } else {
            if (e4_read(v, pb, b) != 0) { kfree(b); return done ? (int)done : -1; }
            memcpy(dst + done, b + in_off, n);
        }
        done += n;
    }
    kfree(b);
    return (int)done;
}

/* Directory iteration: cb(name, nlen, ino, ftype, arg) != 0 stops. */
typedef int (*e4_dcb)(const char *nm, int nl, uint32_t ino, uint8_t ft, void *arg);
static int e4_dir_iter(e4_vol_t *v, const e4_inode_t *d, e4_dcb cb, void *arg) {
    if ((d->mode & S_IFMT_) != S_IFDIR_) return -1;
    if (d->flags & FL_INLINE) {
        uint8_t *tmp = (uint8_t *)kalloc(1024);
        if (!tmp) return -1;
        uint32_t n = e4_inline(v, d, tmp, 1024);
        int rc = 0;
        if (n >= 4 && (rc = cb("..", 2, *(uint32_t *)tmp, 2, arg)) != 0) { kfree(tmp); return rc; }
        /* entries start at 4 in i_block (to 60), then continue in the xattr part */
        for (uint32_t o = 4; o + 8 <= n; ) {
            uint32_t ino = *(uint32_t *)(tmp + o); uint16_t rl = *(uint16_t *)(tmp + o + 4);
            uint8_t nl = tmp[o + 6], ft = tmp[o + 7];
            if (rl < 8 || o + rl > n) break;
            if (ino && nl && o + 8 + nl <= n && (rc = cb((const char *)tmp + o + 8, nl, ino, ft, arg)) != 0) break;
            o += rl;
        }
        kfree(tmp);
        return rc;
    }
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -1;
    int rc = 0;
    uint32_t nblk = (uint32_t)((d->size + v->bs - 1) / v->bs);
    for (uint32_t lb = 0; lb < nblk && !rc; lb++) {
        uint64_t pb = e4_bmap(v, d, lb);
        if (!pb) continue;
        if (e4_read(v, pb, b) != 0) { rc = -1; break; }
        for (uint32_t o = 0; o + 8 <= v->bs; ) {
            uint32_t ino = *(uint32_t *)(b + o); uint16_t rl = *(uint16_t *)(b + o + 4);
            uint8_t nl = b[o + 6], ft = b[o + 7];
            if (rl < 8 || o + rl > v->bs) break;
            if (ino && nl && o + 8 + nl <= v->bs && (rc = cb((const char *)b + o + 8, nl, ino, ft, arg)) != 0) break;
            o += rl;
        }
    }
    kfree(b);
    return rc < 0 ? -1 : rc;
}

typedef struct { const char *want; int wl; uint32_t ino; } e4_find_t;
static int e4_find_cb(const char *nm, int nl, uint32_t ino, uint8_t ft, void *arg) {
    e4_find_t *f = (e4_find_t *)arg;
    if (nl == f->wl && memcmp(nm, f->want, nl) == 0) { f->ino = ino; return 1; }
    return 0;
}

/* Resolve @path (relative to the volume root), following symlinks.
 * Symlink targets: relative to the link's directory; absolute ones are taken
 * relative to the volume root.  Returns inode number or 0. */
static uint32_t e4_namei(e4_vol_t *v, const char *path, e4_inode_t *out, int depth, bool follow_last) {
    if (depth > 8) return 0;
    uint32_t cur = 2, parent = 2;
    if (e4_iget(v, cur, out) != 0) return 0;
    char *tgt = NULL;
    const char *p = path;
    while (*p == '/') p++;
    /* parent stack for ".." */
    uint32_t stack[64]; int sp = 0;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/') p++;
        int n = (int)(p - s);
        while (*p == '/') p++;
        if (n == 1 && s[0] == '.') continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            cur = sp > 0 ? stack[--sp] : 2;
            if (e4_iget(v, cur, out) != 0) return 0;
            continue;
        }
        if ((out->mode & S_IFMT_) != S_IFDIR_) return 0;
        e4_find_t f = { s, n, 0 };
        if (e4_dir_iter(v, out, e4_find_cb, &f) != 1 || !f.ino) return 0;
        parent = cur;
        if (sp < 64) stack[sp++] = cur;
        cur = f.ino;
        if (e4_iget(v, cur, out) != 0) return 0;
        if ((out->mode & S_IFMT_) == S_IFLNK_ && (*p || follow_last)) {
            /* read target */
            if (!tgt && !(tgt = (char *)kalloc(1024))) return 0;
            uint32_t tl = out->size < 1000 ? (uint32_t)out->size : 1000;
            if (out->size < 60 && !(out->flags & (FL_EXTENTS | FL_INLINE)) &&
                *(uint32_t *)(out->raw + 28) == 0) memcpy(tgt, out->iblock, tl);   /* fast symlink */
            else if (e4_pread(v, out, 0, (uint8_t *)tgt, tl) != (int)tl) { kfree(tgt); return 0; }
            tgt[tl] = 0;
            /* build "<dir of link>/<target>/<rest>" */
            char *np = (char *)kalloc(1024);
            if (!np) { kfree(tgt); return 0; }
            int o = 0;
            if (tgt[0] != '/') {
                /* prefix = path up to the link's directory */
                int pl = (int)(s - path);
                if (pl > 500) pl = 500;
                memcpy(np, path, pl); o = pl;
            }
            for (int i = 0; tgt[i] && o < 1000; i++) np[o++] = tgt[i];
            if (*p && o < 1000) { np[o++] = '/'; for (const char *q = p; *q && o < 1020; q++) np[o++] = *q; }
            np[o] = 0;
            kfree(tgt);
            uint32_t r = e4_namei(v, np, out, depth + 1, follow_last);
            kfree(np);
            return r;
        }
    }
    (void)parent;
    if (tgt) kfree(tgt);
    return cur;
}

e4_vol_t *ext4ro_mount(uint8_t dev, uint32_t lba) {
    uint8_t *sb = (uint8_t *)kalloc(1024);
    if (!sb) return NULL;
    if (storage_read_sectors(dev, lba + 2, 2, sb) != 0 || *(uint16_t *)(sb + 56) != 0xEF53) {
        printf("ext4: no ext2/3/4 superblock on device %u at LBA %u\n", dev, lba);
        kfree(sb); return NULL;
    }
    uint32_t inc = *(uint32_t *)(sb + 0x60), roc = *(uint32_t *)(sb + 0x64);
    uint32_t rev = *(uint32_t *)(sb + 0x4C);
    if (inc & ~I_SUPPORTED) {
        printf("ext4: unsupported INCOMPAT features %#x - not mounting\n", inc & ~I_SUPPORTED);
        kfree(sb); return NULL;
    }
    if (inc & I_META_BG) { printf("ext4: meta_bg not supported - not mounting\n"); kfree(sb); return NULL; }
    e4_vol_t *v = (e4_vol_t *)kalloc(sizeof(e4_vol_t));
    if (!v) { kfree(sb); return NULL; }
    memset(v, 0, sizeof(*v));
    v->dev = dev; v->part = lba;
    v->bs = 1024u << *(uint32_t *)(sb + 0x18);
    v->spb = v->bs / 512;
    v->bpg = *(uint32_t *)(sb + 0x20);
    v->ipg = *(uint32_t *)(sb + 0x28);
    v->first_data = *(uint32_t *)(sb + 0x14);
    v->isz = rev >= 1 ? *(uint16_t *)(sb + 0x58) : 128;
    v->dsz = (inc & I_64BIT) ? *(uint16_t *)(sb + 0xFE) : 32;
    if (v->dsz < 32) v->dsz = 32;
    v->blocks = *(uint32_t *)(sb + 0x04) | ((inc & I_64BIT) ? (uint64_t)*(uint32_t *)(sb + 0x150) << 32 : 0);
    v->incompat = inc; v->ro_compat = roc;
    memcpy(v->label, sb + 0x78, 16); v->label[16] = 0;
    kfree(sb);
    if (v->bs > 65536 || v->bpg == 0 || v->ipg == 0 || v->isz < 128) { printf("ext4: bad geometry\n"); kfree(v); return NULL; }
    v->groups = (uint32_t)((v->blocks - v->first_data + v->bpg - 1) / v->bpg);
    uint32_t gbytes = v->groups * v->dsz, gblocks = (gbytes + v->bs - 1) / v->bs;
    v->gdt = (uint8_t *)kalloc(gblocks * v->bs);
    if (!v->gdt) { kfree(v); return NULL; }
    for (uint32_t i = 0; i < gblocks; i++)
        if (e4_read(v, v->first_data + 1 + i, v->gdt + i * v->bs) != 0) { kfree(v->gdt); kfree(v); return NULL; }
    if (inc & I_RECOVER) printf("ext4: journal needs recovery - reading as-is (read-only)\n");
    serial_printf("[ext4ro] mounted dev %u LBA %u label '%s': %u-byte blocks, %u groups, incompat %#x ro_compat %#x\n",
                  dev, lba, v->label, v->bs, v->groups, inc, roc);
    return v;
}

void ext4ro_unmount(e4_vol_t *v) {
    if (!v) return;
    for (int i = 0; i < E4_MAX_VOLS; i++) if (e4_vols[i] == v) e4_vols[i] = NULL;
    kfree(v->gdt); kfree(v);
}
const char *ext4ro_label(const e4_vol_t *v) { return v ? v->label : ""; }
void ext4ro_info(const e4_vol_t *v) {
    if (v) printf("ext4 '%s' on device %u at LBA %u: %u-byte blocks, %u groups (read-only)\n",
                  v->label, v->dev, v->part, v->bs, v->groups);
}

/* ---- VFS adapter ---- */
#define E4_MAX_H 32
typedef struct { bool in_use; e4_vol_t *v; e4_inode_t in; uint64_t pos; } e4_h_t;
static e4_h_t e4_h[E4_MAX_H];

static void *e4_open(vfs_filesystem_t *fs, const char *rel, uint32_t flags) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    if (flags & (VFS_O_WRITE | VFS_O_TRUNC | VFS_O_APPEND | VFS_O_CREATE)) return NULL;
    int s = -1;
    for (int i = 0; i < E4_MAX_H; i++) if (!e4_h[i].in_use) { s = i; break; }
    if (s < 0) return NULL;
    e4_h_t *h = &e4_h[s];
    if (!e4_namei(v, rel, &h->in, 0, true) || (h->in.mode & S_IFMT_) == S_IFDIR_) return NULL;
    h->in_use = true; h->v = v; h->pos = 0;
    return (void *)(uintptr_t)(s + 1);
}
static e4_h_t *e4_hh(void *hd) { int s = (int)(uintptr_t)hd - 1; return (s >= 0 && s < E4_MAX_H && e4_h[s].in_use) ? &e4_h[s] : NULL; }
static int e4_close(vfs_filesystem_t *fs, void *hd) { e4_h_t *h = e4_hh(hd); if (!h) return -1; h->in_use = false; return 0; }
static int e4_readop(vfs_filesystem_t *fs, void *hd, void *buf, uint32_t size) {
    e4_h_t *h = e4_hh(hd); if (!h) return -1;
    int r = e4_pread(h->v, &h->in, h->pos, (uint8_t *)buf, size);
    if (r > 0) h->pos += (uint32_t)r;
    return r;
}
static int e4_write(vfs_filesystem_t *fs, void *hd, const void *b, uint32_t n) { return -1; }
static int e4_seek(vfs_filesystem_t *fs, void *hd, int32_t off, int wh) {
    e4_h_t *h = e4_hh(hd); if (!h) return -1;
    int64_t np = wh == VFS_SEEK_SET ? off : wh == VFS_SEEK_CUR ? (int64_t)h->pos + off :
                 wh == VFS_SEEK_END ? (int64_t)h->in.size + off : -1;
    if (np < 0) return -1;
    h->pos = (uint64_t)np; return (int)np;
}
static int e4_size(vfs_filesystem_t *fs, void *hd) { e4_h_t *h = e4_hh(hd); return h ? (int)h->in.size : -1; }

typedef struct { e4_vol_t *v; vfs_dirent_t *out; uint32_t max, n; const char *dir; } e4_ls_t;
static int e4_ls_cb(const char *nm, int nl, uint32_t ino, uint8_t ft, void *arg) {
    e4_ls_t *l = (e4_ls_t *)arg;
    if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.')) return 0;
    if (l->n >= l->max) return 1;
    vfs_dirent_t *d = &l->out[l->n];
    int k = nl < VFS_MAX_NAME - 1 ? nl : VFS_MAX_NAME - 1;
    memcpy(d->name, nm, k); d->name[k] = 0;
    d->inode = ino;
    e4_inode_t *in = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!in) return -1;
    bool isdir = ft == 2, ok = true;
    uint64_t sz = 0;
    if (e4_iget(l->v, ino, in) == 0) {
        if ((in->mode & S_IFMT_) == S_IFLNK_) {        /* report the target's type */
            static char p[600];
            int o = 0;
            for (const char *q = l->dir; *q && o < 400; q++) p[o++] = *q;
            if (o) p[o++] = '/';
            for (int i = 0; i < k && o < 590; i++) p[o++] = nm[i];
            p[o] = 0;
            if (e4_namei(l->v, p, in, 0, true)) { isdir = (in->mode & S_IFMT_) == S_IFDIR_; sz = in->size; }
            else ok = false;                           /* dangling: hide */
        } else { isdir = (in->mode & S_IFMT_) == S_IFDIR_; sz = in->size; }
    }
    kfree(in);
    if (!ok) return 0;
    d->type = isdir ? VFS_DIRECTORY : VFS_FILE;
    d->size = sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sz;
    l->n++;
    return 0;
}
static int e4_readdir(vfs_filesystem_t *fs, const char *rel, vfs_dirent_t *ents, uint32_t max) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    e4_inode_t *d = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!d) return -1;
    int r = -1;
    if (e4_namei(v, rel ? rel : "", d, 0, true) && (d->mode & S_IFMT_) == S_IFDIR_) {
        e4_ls_t l = { v, ents, max, 0, rel ? rel : "" };
        r = e4_dir_iter(v, d, e4_ls_cb, &l) < 0 ? -1 : (int)l.n;
    }
    kfree(d);
    return r;
}
static int e4_exists(vfs_filesystem_t *fs, const char *rel) {
    e4_inode_t *d = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!d) return 0;
    int ok = e4_namei((e4_vol_t *)fs->fs_data, rel ? rel : "", d, 0, true) != 0;
    kfree(d); return ok;
}
static int e4_ro(vfs_filesystem_t *fs, const char *rel) { return -30; /* EROFS */ }
static int e4_ro2(vfs_filesystem_t *fs, const char *a, const char *b) { return -30; }
static int e4_ro3(vfs_filesystem_t *fs, const char *a, uint64_t l) { return -30; }

static vfs_fs_ops_t e4_ops = {
    .open = e4_open, .close = e4_close, .read = e4_readop, .write = e4_write,
    .seek = e4_seek, .get_size = e4_size, .readdir = e4_readdir,
    .create = e4_ro, .mkdir = e4_ro, .unlink = e4_ro, .exists = e4_exists,
    .rename = e4_ro2, .rmdir = e4_ro, .truncate = e4_ro3,
};
vfs_fs_ops_t *ext4ro_get_ops(void) { return &e4_ops; }

int ext4ro_vfs_register(const char *path, e4_vol_t *v) {
    int r = vfs_mount(path, &e4_ops, v);
    if (r == 0) {
        size_t l = strlen(path); if (l >= sizeof(v->mpath)) l = sizeof(v->mpath) - 1;
        memcpy(v->mpath, path, l); v->mpath[l] = 0;
        for (int i = 0; i < E4_MAX_VOLS; i++) if (!e4_vols[i]) { e4_vols[i] = v; break; }
    }
    return r;
}
e4_vol_t *ext4ro_find_mounted(uint8_t dev, uint32_t lba, const char **path) {
    for (int i = 0; i < E4_MAX_VOLS; i++)
        if (e4_vols[i] && e4_vols[i]->dev == dev && e4_vols[i]->part == lba) { if (path) *path = e4_vols[i]->mpath; return e4_vols[i]; }
    return NULL;
}
