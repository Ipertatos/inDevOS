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
    uint32_t incompat, ro_compat, compat;
    bool     rw;                    /* read-write enabled (ext4ro_enable_rw) */
    bool     sb_clean;              /* on-disk s_state has VALID_FS set */
    int32_t  alloc_since_sync;      /* net blocks allocated, not yet in s_free_blocks_count */
    int32_t  ialloc_since_sync;     /* net inodes allocated, not yet in s_free_inodes_count */
    uint32_t first_ino;
    uint16_t want_extra;
    uint64_t blocks;
    char     label[17];
    char     mpath[64];
    uint8_t *gdt;                   /* all group descriptors */
    bool     csum;                  /* metadata_csum: verify crc32c */
    uint32_t seed;                  /* crc32c(~0, uuid) or s_checksum_seed */
    uint32_t csum_errs;
    uint32_t hseed[4];              /* s_hash_seed (dir_index) */
    uint8_t  hunsigned;             /* 3 if EXT2_FLAGS_UNSIGNED_HASH (version += 3) */
    struct e4_ovl *ovl;             /* read-only mount of a volume needing recovery: */
    uint32_t novl;                  /*   committed journal blocks, sorted by home block */
    struct e4_jw *jw;               /* journaled writes (JBD2 transaction layer) */
    bool     jtest;                 /* test-only: allow rw on a journaled volume */
    uint32_t jres, jtxmax;          /* test-only journal knobs (0 = defaults) */
    bool     closing;               /* unmount in progress: final clean superblock */
};
struct e4_ovl { uint64_t home, phys; uint32_t esc; };

typedef struct {
    uint16_t mode;
    uint32_t uid, gid, flags, links;
    uint64_t size;
    uint32_t mtime;
    uint32_t ino, cseed;            /* inode number, per-inode crc32c seed */
    uint8_t  iblock[60];
    uint8_t  raw[1024];             /* whole on-disk inode (inline data xattr) */
    uint32_t rawlen;
} e4_inode_t;

#define RO_HUGE_FILE     0x0008
#define RO_METADATA_CSUM 0x0400
#define FL_INDEX     0x00001000u
#define FL_HUGE_FILE 0x00040000u
#define E4_BMAP_ERR  ((uint64_t)-1)

/* crc32c (Castagnoli, reflected 0x82F63B78) without the final inversion -
 * the form ext4 stores (Linux ext4_chksum). */
static uint32_t e4_crc_tab[256];
static volatile bool e4_crc_ready;
static uint32_t e4_crc32c(uint32_t crc, const void *data, uint32_t len) {
    if (!e4_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
            e4_crc_tab[i] = c;
        }
        e4_crc_ready = true;
    }
    const uint8_t *p = (const uint8_t *)data;
    while (len--) crc = e4_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}
static const uint8_t e4_zero4[4];
static void e4_csum_bad(e4_vol_t *v, const char *what, uint64_t id, uint32_t want, uint32_t got) {
    v->csum_errs++;
    serial_printf("[ext4ro] %s %llu checksum mismatch (stored 0x%x computed 0x%x) - EIO\n",
                  what, (unsigned long long)id, want, got);
    if (v->csum_errs <= 4)
        printf("ext4: %s %llu: metadata checksum mismatch\n", what, (unsigned long long)id);
}

#define E4_MAX_VOLS 8
static e4_vol_t *e4_vols[E4_MAX_VOLS];
/* ---- journaled writes (JBD2, ordered mode) ----
 * One transaction per VFS operation.  Every metadata write (anything that goes
 * through e4_wblk / e4_swrite: bitmaps, group descriptors, inode tables,
 * directory, extent and indirect blocks) lands in an in-memory block cache
 * instead of its home location; file data (e4_wdata) is written in place
 * immediately, i.e. before the commit (ordered mode).  At the end of the
 * operation the cached blocks are logged (revoke blocks, descriptor + copies,
 * commit block last).  Home locations are written only by a checkpoint, which
 * runs at the start of a transaction when the log or the cache is getting full
 * (forced checkpoint), and at sync/unmount; a checkpoint never runs while a
 * transaction is open.  Freed blocks that have a copy in the live log are
 * revoked in the freeing transaction (and the revoke cancelled if the same
 * transaction journals the block again). */
#define E4_CB_RUN  1
#define E4_CB_DONE 2
#define E4_JW_MAXCB 512
struct e4_cb { uint64_t blk; uint8_t *buf, *cbuf; uint8_t st, fr; };   /* cbuf: committed version of a RUN block not yet home; fr: freed by the running transaction */
struct e4_jw {
    e4_inode_t jin;
    uint64_t *jmap;
    uint32_t first, last, seq, tseq, head, start, seed, tagsz, reserve;
    bool c2, c3, b64;
    uint8_t uuid[16];
    struct e4_cb *cb; uint32_t ncb, cbcap, ndone;
    uint64_t *rv; uint32_t nrv, rvcap;
    uint64_t *lg; uint32_t nlg, lgcap;
    int depth;
    bool broken;
    uint32_t ncommit, nckpt, nforced, nrevoke, nlogged, nwrap, naborted, txmax;
    int32_t  snap_a, snap_i;
    uint64_t *pfs, *pfc; uint32_t npf, pfcap, pfcap2;   /* frees deferred to the end of the transaction */
    bool     applying;
    uint32_t ndeferred, norphan;
    bool     aborted;
};
static int e4_cb_find(struct e4_jw *w, uint64_t blk) {
    for (uint32_t i = 0; i < w->ncb; i++) if (w->cb[i].blk == blk) return (int)i;
    return -1;
}
static void e4_cb_drop(struct e4_jw *w, uint32_t i) {
    if (w->cb[i].st == E4_CB_DONE) w->ndone--;
    if (w->cb[i].cbuf) kfree(w->cb[i].cbuf);
    kfree(w->cb[i].buf); w->cb[i] = w->cb[--w->ncb];
}
static bool e4_u64_has(const uint64_t *a, uint32_t n, uint64_t x) { for (uint32_t i = 0; i < n; i++) if (a[i] == x) return true; return false; }
static void e4_u64_del(uint64_t *a, uint32_t *n, uint64_t x) { for (uint32_t i = 0; i < *n; i++) if (a[i] == x) { a[i] = a[--*n]; return; } }
static int e4_u64_add(uint64_t **a, uint32_t *n, uint32_t *cap, uint64_t x) {
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 128;
        uint64_t *b = (uint64_t *)kalloc(sizeof(uint64_t) * nc);
        if (!b) return -1;
        if (*a) { memcpy(b, *a, sizeof(uint64_t) * *n); kfree(*a); }
        *a = b; *cap = nc;
    }
    (*a)[(*n)++] = x;
    return 0;
}
static int e4_sread(e4_vol_t *v, uint64_t lba, uint32_t n, void *buf) {
    struct e4_jw *w = v->jw;
    if (!w || !w->ncb || lba < v->part) return storage_read_sectors(v->dev, lba, n, buf);
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        uint64_t rel = lba - v->part, blk = rel / v->spb; uint32_t off = (uint32_t)(rel % v->spb), k = v->spb - off;
        if (k > n) k = n;
        int i = e4_cb_find(w, blk);
        if (i >= 0) memcpy(p, w->cb[i].buf + off * 512, k * 512);
        else if (storage_read_sectors(v->dev, lba, k, p) != 0) return -1;
        lba += k; p += k * 512; n -= k;
    }
    return 0;
}
static void e4_tx_begin(e4_vol_t *v);
static int e4_tx_end(e4_vol_t *v);
static int e4_swrite(e4_vol_t *v, uint64_t lba, uint32_t n, const void *buf) {
    struct e4_jw *w = v->jw;
    if (!w || lba < v->part) return storage_write_sectors(v->dev, lba, n, buf);
    bool implicit = w->depth == 0;
    if (implicit) e4_tx_begin(v);
    const uint8_t *p = (const uint8_t *)buf; int rc = 0;
    while (n) {
        uint64_t rel = lba - v->part, blk = rel / v->spb; uint32_t off = (uint32_t)(rel % v->spb), k = v->spb - off;
        if (k > n) k = n;
        int i = e4_cb_find(w, blk);
        if (i < 0) {
            if (w->ncb == w->cbcap) {
                uint32_t nc = w->cbcap ? w->cbcap * 2 : 64;
                struct e4_cb *c = (struct e4_cb *)kalloc(sizeof(struct e4_cb) * nc);
                if (!c) { rc = -1; break; }
                if (w->cb) { memcpy(c, w->cb, sizeof(struct e4_cb) * w->ncb); kfree(w->cb); }
                w->cb = c; w->cbcap = nc;
            }
            uint8_t *b = (uint8_t *)kalloc(v->bs);
            if (!b) { rc = -1; break; }
            if (k < v->spb && storage_read_sectors(v->dev, v->part + blk * v->spb, v->spb, b) != 0) { kfree(b); rc = -1; break; }
            i = (int)w->ncb++; w->cb[i].blk = blk; w->cb[i].buf = b; w->cb[i].cbuf = NULL; w->cb[i].st = 0; w->cb[i].fr = 0;
        }
        if (w->cb[i].st == E4_CB_DONE) {                 /* keep the committed version until commit/abort */
            uint8_t *c = (uint8_t *)kalloc(v->bs);
            if (!c) { rc = -1; w->broken = true; break; }
            memcpy(c, w->cb[i].buf, v->bs); w->cb[i].cbuf = c; w->ndone--;
        }
        w->cb[i].fr = 0;
        memcpy(w->cb[i].buf + off * 512, p, k * 512);
        w->cb[i].st = E4_CB_RUN;
        e4_u64_del(w->rv, &w->nrv, blk);                 /* journaled again: cancel its revoke */
        lba += k; p += k * 512; n -= k;
    }
    if (implicit && e4_tx_end(v) != 0) rc = -1;
    return rc;
}
/* blocks freed by the running transaction */
static void e4_jw_freed(e4_vol_t *v, uint64_t start, uint64_t cnt) {
    struct e4_jw *w = v->jw;
    if (!w) return;
    /* a committed version not yet home must survive until the free commits (or is aborted) */
    for (uint32_t i = 0; i < w->ncb; ) {
        if (w->cb[i].blk >= start && w->cb[i].blk - start < cnt) {
            if (w->cb[i].st == E4_CB_DONE || w->cb[i].cbuf) { w->cb[i].fr = 1; i++; } else e4_cb_drop(w, i);
        } else i++;
    }
    for (uint32_t i = 0; i < w->nlg; i++)
        if (w->lg[i] >= start && w->lg[i] - start < cnt && !e4_u64_has(w->rv, w->nrv, w->lg[i])) {
            if (e4_u64_add(&w->rv, &w->nrv, &w->rvcap, w->lg[i]) != 0) w->broken = true;
            else w->nrevoke++;
        }
}
/* A block freed by the running transaction must not be reused before that free commits
 * (jbd2 semantics): the bitmap update itself is deferred to the end of the transaction,
 * so the allocator cannot see the block as free while the transaction runs. */
static bool e4_jw_defer(e4_vol_t *v, uint64_t start, uint64_t cnt) {
    struct e4_jw *w = v->jw;
    if (!w || !w->depth || w->applying) return false;
    e4_jw_freed(v, start, cnt);
    if (e4_u64_add(&w->pfs, &w->npf, &w->pfcap, start) != 0) { w->broken = true; return true; }
    w->npf--;
    if (e4_u64_add(&w->pfc, &w->npf, &w->pfcap2, cnt) != 0) { w->broken = true; return true; }
    w->ndeferred++;
    return true;
}
/* file data: in place, now (ordered mode) */
static int e4_wdata(e4_vol_t *v, uint64_t blk, const void *buf) {
    if (!v->rw || blk < v->first_data + 1 || blk >= v->blocks) return -1;
    if (v->jw) { int i = e4_cb_find(v->jw, blk); if (i >= 0) e4_cb_drop(v->jw, (uint32_t)i); }
    return storage_write_sectors(v->dev, v->part + blk * v->spb, v->spb, buf);
}

static int e4_read(e4_vol_t *v, uint64_t blk, void *buf) {
    if (v->novl) {
        uint32_t lo = 0, hi = v->novl;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (v->ovl[m].home < blk) lo = m + 1; else hi = m; }
        if (lo < v->novl && v->ovl[lo].home == blk) {
            int r = storage_read_sectors(v->dev, v->part + v->ovl[lo].phys * v->spb, v->spb, buf);
            if (r == 0 && v->ovl[lo].esc) { uint8_t *b = (uint8_t *)buf; b[0] = 0xC0; b[1] = 0x3B; b[2] = 0x39; b[3] = 0x98; }
            return r;
        }
    }
    return e4_sread(v, v->part + blk * v->spb, v->spb, buf);
}
static int e4_wblk(e4_vol_t *v, uint64_t blk, const void *buf) {
    if (!v->rw || blk < v->first_data + 1 || blk >= v->blocks) return -1;   /* never the superblock area */
    return e4_swrite(v, v->part + blk * v->spb, v->spb, buf);
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

/* Inode checksum (metadata_csum): crc32c over the inode with the checksum
 * fields treated as zero.  Returns the value to store (16 bits when there is
 * no i_checksum_hi); *stored gets the on-disk value, *seed the inode seed. */
static uint32_t e4_inode_csum(e4_vol_t *v, uint32_t ino, const uint8_t *r, uint32_t isz,
                              uint32_t *seed, uint32_t *stored) {
    uint32_t le = ino, gen = *(uint32_t *)(r + 0x64);
    uint32_t s = e4_crc32c(v->seed, &le, 4);
    s = e4_crc32c(s, &gen, 4);
    if (seed) *seed = s;
    bool has_hi = isz > 128 && 128u + *(uint16_t *)(r + 0x80) >= 0x84;
    uint32_t c = e4_crc32c(s, r, 0x7C);
    c = e4_crc32c(c, e4_zero4, 2);
    if (isz > 128) {
        c = e4_crc32c(c, r + 0x7E, 0x82 - 0x7E);
        if (has_hi) { c = e4_crc32c(c, e4_zero4, 2); c = e4_crc32c(c, r + 0x84, isz - 0x84); }
        else c = e4_crc32c(c, r + 0x82, isz - 0x82);
    } else {
        c = e4_crc32c(c, r + 0x7E, isz - 0x7E);
    }
    if (stored) *stored = *(uint16_t *)(r + 0x7C) | (has_hi ? (uint32_t)*(uint16_t *)(r + 0x82) << 16 : 0);
    return has_hi ? c : (c & 0xFFFF);
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
    out->ino = ino;
    if (v->csum) {
        uint32_t stored, calc = e4_inode_csum(v, ino, r, out->rawlen, &out->cseed, &stored);
        if (stored != calc) { e4_csum_bad(v, "inode", ino, stored, calc); return -1; }
    }
    return 0;
}

/* logical -> physical block (0 = hole / unwritten). */
static uint64_t e4_bmap_x(e4_vol_t *v, const e4_inode_t *in, uint32_t lblk, bool *unw_out);
static uint64_t e4_bmap(e4_vol_t *v, const e4_inode_t *in, uint32_t lblk) { return e4_bmap_x(v, in, lblk, NULL); }
/* @unw_out != NULL: also return the physical block of an unwritten extent (*unw_out = true) */
static uint64_t e4_bmap_x(e4_vol_t *v, const e4_inode_t *in, uint32_t lblk, bool *unw_out) {
    if (unw_out) *unw_out = false;
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
                        if (!unwritten || unw_out) {
                            uint64_t start = *(uint32_t *)(e + 8) | ((uint64_t)*(uint16_t *)(e + 6) << 32);
                            res = start + (lblk - lb);
                            if (unwritten) *unw_out = true;
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
            if (e4_read(v, leaf, buf) != 0) { res = E4_BMAP_ERR; break; }
            if (*(uint16_t *)buf != 0xF30A) { res = E4_BMAP_ERR; break; }
            if (v->csum) {
                uint32_t to = 12 + 12u * *(uint16_t *)(buf + 4);
                if (to + 4 > v->bs) { res = E4_BMAP_ERR; break; }
                uint32_t c = e4_crc32c(in->cseed, buf, to), st = *(uint32_t *)(buf + to);
                if (c != st) { e4_csum_bad(v, "extent block", leaf, st, c); res = E4_BMAP_ERR; break; }
            }
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
    else { lblk -= per * per; lvl = 3; root = ib[14]; if ((uint64_t)lblk >= (uint64_t)per * per * per) return 0; }
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
        if (pb == E4_BMAP_ERR) { kfree(b); return done ? (int)done : -5 /* -EIO */; }
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

/* metadata_csum directory blocks: leaves end in a 12-byte fake dirent
 * (inode 0, rec_len 12, file_type 0xDE) holding crc32c(inode seed, block);
 * htree index blocks (dx root / dx node) end in a dx_tail after the
 * dx_entry array. */
static bool e4_dirblk_ok(e4_vol_t *v, const e4_inode_t *d, uint32_t lb, uint64_t pb, const uint8_t *b) {
    uint32_t bs = v->bs;
    const uint8_t *t = b + bs - 12;
    if (*(uint32_t *)t == 0 && *(uint16_t *)(t + 4) == 12 && t[6] == 0 && t[7] == 0xDE) {
        uint32_t c = e4_crc32c(d->cseed, b, bs - 12), st = *(uint32_t *)(t + 8);
        if (c != st) { e4_csum_bad(v, "dir block", pb, st, c); return false; }
        return true;
    }
    uint32_t co = 0;
    if ((d->flags & FL_INDEX) && lb == 0) co = 24 + b[24 + 5];        /* dx_root: . .. info */
    else if (*(uint32_t *)b == 0 && *(uint16_t *)(b + 4) == bs) co = 8;  /* dx_node */
    if (!co) { e4_csum_bad(v, "dir block (no csum tail)", pb, 0, 0); return false; }
    uint32_t limit = *(uint16_t *)(b + co), count = *(uint16_t *)(b + co + 2);
    uint32_t to = co + limit * 8;
    if (count > limit || to + 8 > bs) { e4_csum_bad(v, "dx block (bad limit)", pb, limit, count); return false; }
    uint32_t c = e4_crc32c(d->cseed, b, co + count * 8);
    c = e4_crc32c(c, b + to, 4);
    c = e4_crc32c(c, e4_zero4, 4);                                  /* dt_checksum as zero */
    uint32_t st = *(uint32_t *)(b + to + 4);
    if (c != st) { e4_csum_bad(v, "dx block", pb, st, c); return false; }
    return true;
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
        if (pb == E4_BMAP_ERR) { rc = -1; break; }
        if (!pb) continue;
        if (e4_read(v, pb, b) != 0) { rc = -1; break; }
        if (v->csum && !e4_dirblk_ok(v, d, lb, pb, b)) { rc = -1; break; }
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


/* ---- htree (dir_index): hashes (e2fsprogs/Linux dirhash) and index lookup ---- */
static void e4_tea(uint32_t buf[4], const uint32_t in[]) {
    uint32_t sum = 0, b0 = buf[0], b1 = buf[1], a = in[0], b = in[1], c = in[2], d = in[3];
    for (int n = 16; n; n--) {
        sum += 0x9E3779B9u;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    }
    buf[0] += b0; buf[1] += b1;
}
#define E4F_(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define E4G_(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define E4H_(x, y, z) ((x) ^ (y) ^ (z))
#define E4R_(f, a, b, c, d, x, s) (a += f(b, c, d) + (x), a = (a << (s)) | (a >> (32 - (s))))
static void e4_md4h(uint32_t buf[4], const uint32_t in[]) {
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
    const uint32_t K2 = 013240474631u, K3 = 015666365641u;
    E4R_(E4F_, a, b, c, d, in[0], 3);  E4R_(E4F_, d, a, b, c, in[1], 7);
    E4R_(E4F_, c, d, a, b, in[2], 11); E4R_(E4F_, b, c, d, a, in[3], 19);
    E4R_(E4F_, a, b, c, d, in[4], 3);  E4R_(E4F_, d, a, b, c, in[5], 7);
    E4R_(E4F_, c, d, a, b, in[6], 11); E4R_(E4F_, b, c, d, a, in[7], 19);
    E4R_(E4G_, a, b, c, d, in[1] + K2, 3);  E4R_(E4G_, d, a, b, c, in[3] + K2, 5);
    E4R_(E4G_, c, d, a, b, in[5] + K2, 9);  E4R_(E4G_, b, c, d, a, in[7] + K2, 13);
    E4R_(E4G_, a, b, c, d, in[0] + K2, 3);  E4R_(E4G_, d, a, b, c, in[2] + K2, 5);
    E4R_(E4G_, c, d, a, b, in[4] + K2, 9);  E4R_(E4G_, b, c, d, a, in[6] + K2, 13);
    E4R_(E4H_, a, b, c, d, in[3] + K3, 3);  E4R_(E4H_, d, a, b, c, in[7] + K3, 9);
    E4R_(E4H_, c, d, a, b, in[2] + K3, 11); E4R_(E4H_, b, c, d, a, in[6] + K3, 15);
    E4R_(E4H_, a, b, c, d, in[1] + K3, 3);  E4R_(E4H_, d, a, b, c, in[5] + K3, 9);
    E4R_(E4H_, c, d, a, b, in[0] + K3, 11); E4R_(E4H_, b, c, d, a, in[4] + K3, 15);
    buf[0] += a; buf[1] += b; buf[2] += c; buf[3] += d;
}
static int e4_hc(const char *m, int i, int uns) { return uns ? (int)((const unsigned char *)m)[i] : (int)((const signed char *)m)[i]; }
static void e4_s2h(const char *msg, int len, uint32_t *buf, int num, int uns) {
    uint32_t pad = (uint32_t)len | ((uint32_t)len << 8), val;
    pad |= pad << 16; val = pad;
    if (len > num * 4) len = num * 4;
    for (int i = 0; i < len; i++) {
        val = (uint32_t)e4_hc(msg, i, uns) + (val << 8);
        if ((i % 4) == 3) { *buf++ = val; val = pad; num--; }
    }
    if (--num >= 0) *buf++ = val;
    while (--num >= 0) *buf++ = pad;
}
/* major hash (low bit clear) of @nm for on-disk hash version @ver; -95 for unsupported (siphash) */
static int e4_dxhash(e4_vol_t *v, int ver, const char *nm, int nl, uint32_t *out) {
    uint32_t buf[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u }, in[8], h = 0;
    if (v->hseed[0] | v->hseed[1] | v->hseed[2] | v->hseed[3]) memcpy(buf, v->hseed, 16);
    if (ver <= 2) ver += v->hunsigned;
    int uns = ver >= 3, len = nl; const char *p = nm;
    switch (ver) {
    case 0: case 3: {
        uint32_t h0 = 0x12a3fe2d, h1 = 0x37abe8f9;
        for (int i = 0; i < nl; i++) {
            uint32_t x = h1 + (h0 ^ (uint32_t)(e4_hc(nm, i, uns) * 7152373));
            if (x & 0x80000000u) x -= 0x7fffffff;
            h1 = h0; h0 = x;
        }
        h = h0 << 1; break;
    }
    case 1: case 4:
        while (len > 0) { e4_s2h(p, len, in, 8, uns); e4_md4h(buf, in); len -= 32; p += 32; }
        h = buf[1]; break;
    case 2: case 5:
        while (len > 0) { e4_s2h(p, len, in, 4, uns); e4_tea(buf, in); len -= 16; p += 16; }
        h = buf[0]; break;
    default: return -95;
    }
    *out = h & ~1u;
    return 0;
}
#define E4_DXB(b, co, i) (*(uint32_t *)((b) + (co) + 8 * (i) + 4) & 0x0FFFFFFFu)
#define E4_DXH(b, co, i) (*(uint32_t *)((b) + (co) + 8 * (i)))
#define E4_DXLIM(b, co)  (*(uint16_t *)((b) + (co)))
#define E4_DXCNT(b, co)  (*(uint16_t *)((b) + (co) + 2))
static uint32_t e4_dx_limit(e4_vol_t *v, bool root) { return (v->bs - (root ? 32 : 8)) / 8 - (v->csum ? 1 : 0); }
static void e4_dx_csum(e4_vol_t *v, const e4_inode_t *d, uint8_t *b, uint32_t co) {
    if (!v->csum) return;
    uint32_t to = co + E4_DXLIM(b, co) * 8;
    uint32_t c = e4_crc32c(d->cseed, b, co + E4_DXCNT(b, co) * 8);
    c = e4_crc32c(c, b + to, 4);
    c = e4_crc32c(c, e4_zero4, 4);
    *(uint32_t *)(b + to + 4) = c;
}
/* last entry whose hash <= h (entry 0 covers everything below entry 1) */
static uint32_t e4_dx_pick(const uint8_t *b, uint32_t co, uint32_t h) {
    int lo = 1, hi = (int)E4_DXCNT(b, co) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (E4_DXH(b, co, m) > h) hi = m - 1; else lo = m + 1; }
    return (uint32_t)(lo - 1);
}
/* read + validate an index block: root (lb 0) or node */
static int e4_dx_load(e4_vol_t *v, const e4_inode_t *d, uint32_t lb, bool root, uint8_t *b, uint64_t *pbo) {
    uint32_t nblk = (uint32_t)(d->size / v->bs);
    if (lb >= nblk) return -5;
    uint64_t pb = e4_bmap(v, d, lb);
    if (pb == E4_BMAP_ERR || !pb || e4_read(v, pb, b) != 0) return -5;
    if (v->csum && !e4_dirblk_ok(v, d, lb, pb, b)) return -5;
    uint32_t co = root ? 32 : 8;
    if (root) { if (b[29] != 8 || *(uint32_t *)(b + 24) != 0) return -5; }
    else if (*(uint32_t *)b != 0 || *(uint16_t *)(b + 4) != v->bs) return -5;
    if (E4_DXLIM(b, co) != e4_dx_limit(v, root) || E4_DXCNT(b, co) == 0 || E4_DXCNT(b, co) > E4_DXLIM(b, co)) return -5;
    if (pbo) *pbo = pb;
    return 0;
}
/* Find @nm through the index.  1 found (*ino), 0 not present, <0 index unusable. */
static int e4_dx_find(e4_vol_t *v, const e4_inode_t *d, const char *nm, int nl, uint32_t *ino) {
    if (!(d->flags & FL_INDEX) || (d->flags & FL_INLINE)) return -22;
    uint8_t *rb = (uint8_t *)kalloc(v->bs), *nb = (uint8_t *)kalloc(v->bs), *lb = (uint8_t *)kalloc(v->bs);
    int rc = -12;
    if (!rb || !nb || !lb) goto out;
    if ((rc = e4_dx_load(v, d, 0, true, rb, NULL)) != 0) goto out;
    uint32_t lv = rb[30], h;
    if (lv > 1) { rc = -95; goto out; }
    if ((rc = e4_dxhash(v, rb[28], nm, nl, &h)) != 0) goto out;
    uint32_t ri = e4_dx_pick(rb, 32, h), ni = 0, nco = 32;
    uint8_t *ib = rb;
    if (lv) {
        if ((rc = e4_dx_load(v, d, E4_DXB(rb, 32, ri), false, nb, NULL)) != 0) goto out;
        ib = nb; nco = 8; ni = e4_dx_pick(nb, 8, h);
    } else ni = ri;
    for (int hops = 0; hops < 64; hops++) {
        uint32_t llb = E4_DXB(ib, nco, ni);
        uint64_t pb = llb < d->size / v->bs ? e4_bmap(v, d, llb) : E4_BMAP_ERR;
        if (pb == E4_BMAP_ERR || !pb || e4_read(v, pb, lb) != 0) { rc = -5; goto out; }
        if (v->csum && !e4_dirblk_ok(v, d, llb, pb, lb)) { rc = -5; goto out; }
        for (uint32_t o = 0; o + 8 <= v->bs; ) {
            uint32_t ei = *(uint32_t *)(lb + o); uint16_t rl = *(uint16_t *)(lb + o + 4);
            if (rl < 8 || o + rl > v->bs) { rc = -5; goto out; }
            if (ei && lb[o + 6] == nl && o + 8 + nl <= v->bs && !memcmp(lb + o + 8, nm, nl)) { *ino = ei; rc = 1; goto out; }
            o += rl;
        }
        /* hash collision continued into the next block? (low bit of the next hash) */
        uint32_t nh;
        if (ni + 1 < E4_DXCNT(ib, nco)) { ni++; nh = E4_DXH(ib, nco, ni); }
        else if (lv && ri + 1 < E4_DXCNT(rb, 32)) {
            ri++; nh = E4_DXH(rb, 32, ri);
            if ((nh & 1) && (nh & ~1u) == h) { if ((rc = e4_dx_load(v, d, E4_DXB(rb, 32, ri), false, nb, NULL)) != 0) goto out; ni = 0; }
        } else break;
        if (!(nh & 1) || (nh & ~1u) != h) break;
    }
    rc = 0;
out:
    if (rb) kfree(rb);
    if (nb) kfree(nb);
    if (lb) kfree(lb);
    return rc;
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
        int dr = (out->flags & FL_INDEX) ? e4_dx_find(v, out, s, n, &f.ino) : -1;
        if (dr == 0) return 0;
        if (dr < 0 && (e4_dir_iter(v, out, e4_find_cb, &f) != 1 || !f.ino)) return 0;
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


/* ---- JBD2 journal recovery (internal journal inode only) ----
 * Same passes as jbd2/e2fsprogs recovery.c: SCAN finds the end of the last
 * committed transaction (a bad commit/descriptor/revoke checksum or a
 * sequence break ends the log), REVOKE collects revoke records, VERIFY checks
 * every tag checksum and target before anything is written, REPLAY copies the
 * journalled blocks home in log order (or OVERLAY records them in memory). */
#define JBD2_MAGIC 0xC03B3998u
enum { J_SCAN, J_REVOKE, J_VERIFY, J_REPLAY, J_OVERLAY };
static uint32_t j_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t j_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void j_put32(uint8_t *p, uint32_t x) { p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16); p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x; }
typedef struct { uint64_t blk; uint32_t seq; } e4_jrv_t;
typedef struct {
    e4_vol_t *v; e4_inode_t jin;
    uint32_t first, last, start, seq, end, inc, seed, tagsz, jsb_errno;
    bool c2, c3, b64;
    e4_jrv_t *rv; uint32_t nrv, rvcap;
    uint32_t ntags, nrevoked, nwritten;
    const char *why;
} e4_j_t;
static int e4_j_read(e4_j_t *j, uint32_t jb, uint8_t *buf, uint64_t *phys) {
    uint64_t pb = e4_bmap(j->v, &j->jin, jb);
    if (!pb || pb == E4_BMAP_ERR || pb >= j->v->blocks) return -1;
    if (phys) *phys = pb;
    return storage_read_sectors(j->v->dev, j->v->part + pb * j->v->spb, j->v->spb, buf);
}
static uint32_t e4_j_next(e4_j_t *j, uint32_t b) { return ++b >= j->last ? j->first : b; }
static bool e4_j_tail_ok(e4_j_t *j, uint8_t *buf) {        /* descriptor / revoke block tail */
    uint32_t bs = j->v->bs, st = j_be32(buf + bs - 4);
    uint8_t save[4]; memcpy(save, buf + bs - 4, 4); memset(buf + bs - 4, 0, 4);
    uint32_t c = e4_crc32c(j->seed, buf, bs);
    memcpy(buf + bs - 4, save, 4);
    return c == st;
}
static bool e4_j_revoked(e4_j_t *j, uint64_t blk, uint32_t seq) {
    for (uint32_t i = 0; i < j->nrv; i++) if (j->rv[i].blk == blk) return (int32_t)(j->rv[i].seq - seq) >= 0;
    return false;
}
static int e4_j_addrv(e4_j_t *j, uint64_t blk, uint32_t seq) {
    for (uint32_t i = 0; i < j->nrv; i++) if (j->rv[i].blk == blk) { if ((int32_t)(seq - j->rv[i].seq) > 0) j->rv[i].seq = seq; return 0; }
    if (j->nrv == j->rvcap) {
        uint32_t nc = j->rvcap ? j->rvcap * 2 : 256;
        e4_jrv_t *n = (e4_jrv_t *)kalloc(sizeof(e4_jrv_t) * nc);
        if (!n) return -1;
        if (j->rv) { memcpy(n, j->rv, sizeof(e4_jrv_t) * j->nrv); kfree(j->rv); }
        j->rv = n; j->rvcap = nc;
    }
    j->rv[j->nrv].blk = blk; j->rv[j->nrv].seq = seq; j->nrv++;
    return 0;
}
static int e4_ovl_add(e4_vol_t *v, uint32_t cap, uint64_t home, uint64_t phys, bool esc) {
    uint32_t lo = 0, hi = v->novl;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (v->ovl[m].home < home) lo = m + 1; else hi = m; }
    if (lo < v->novl && v->ovl[lo].home == home) { v->ovl[lo].phys = phys; v->ovl[lo].esc = esc; return 0; }
    if (v->novl >= cap) return -1;
    memmove(v->ovl + lo + 1, v->ovl + lo, sizeof(struct e4_ovl) * (v->novl - lo));
    v->ovl[lo].home = home; v->ovl[lo].phys = phys; v->ovl[lo].esc = esc; v->novl++;
    return 0;
}
/* one pass over the log; 0 ok, <0 error (j->why says what) */
static int e4_j_pass(e4_j_t *j, int pass, uint8_t *buf, uint8_t *db, uint32_t ovcap) {
    e4_vol_t *v = j->v;
    uint32_t bs = v->bs, seq = j->seq, blk = j->start;
    bool cs = j->c2 || j->c3;
    for (uint32_t guard = 0; ; guard++) {
        if (pass != J_SCAN && seq == j->end) break;
        if (guard > j->last) { j->why = "journal loops"; return -1; }
        if (e4_j_read(j, blk, buf, NULL) != 0) { j->why = "journal block unreadable"; return -1; }
        if (j_be32(buf) != JBD2_MAGIC || j_be32(buf + 8) != seq) {
            if (pass == J_SCAN) break;
            j->why = "log changed between passes"; return -1;
        }
        uint32_t type = j_be32(buf + 4);
        if (type == 1) {                                      /* descriptor */
            if (cs && !e4_j_tail_ok(j, buf)) { if (pass == J_SCAN) break; j->why = "descriptor block checksum"; return -1; }
            uint8_t *p = buf + 12, *end = buf + bs - (cs ? 4 : 0);
            while (p + j->tagsz <= end) {
                uint32_t fl = j->c3 ? j_be32(p + 4) : j_be16(p + 6);
                uint64_t home = j_be32(p) | (j->b64 ? (uint64_t)j_be32(p + 8) << 32 : 0);
                blk = e4_j_next(j, blk);
                if (pass >= J_VERIFY) {
                    j->ntags++;
                    if (!e4_j_revoked(j, home, seq)) {
                        uint64_t phys = 0;
                        if (e4_j_read(j, blk, db, &phys) != 0) { j->why = "journal data block unreadable"; return -1; }
                        if (pass == J_VERIFY) {
                            if (home >= v->blocks) { j->why = "tag points outside the volume"; return -1; }
                            if (cs) {
                                uint8_t sq[4]; j_put32(sq, seq);
                                uint32_t c = e4_crc32c(e4_crc32c(j->seed, sq, 4), db, bs);
                                uint32_t want = j->c3 ? j_be32(p + 12) : j_be16(p + 4);
                                if ((j->c3 ? c : (c & 0xFFFF)) != want) { j->why = "data block checksum"; return -1; }
                            }
                        } else if (pass == J_REPLAY) {
                            if (fl & 1) j_put32(db, JBD2_MAGIC);
                            if (storage_write_sectors(v->dev, v->part + home * v->spb, v->spb, db) != 0) { j->why = "write failed"; return -1; }
                            j->nwritten++;
                        } else if (pass == J_OVERLAY) {
                            if (e4_ovl_add(v, ovcap, home, phys, (fl & 1) != 0) != 0) { j->why = "overlay full"; return -1; }
                        }
                    } else if (pass == J_VERIFY) j->nrevoked++;
                }
                p += j->tagsz;
                if (!(fl & 2)) p += 16;                       /* no SAME_UUID: uuid follows */
                if (fl & 8) break;                            /* LAST_TAG */
            }
            blk = e4_j_next(j, blk);
        } else if (type == 2) {                               /* commit */
            if (pass == J_SCAN && cs) {
                memcpy(db, buf, bs); memset(db + 0x10, 0, 4);
                if (e4_crc32c(j->seed, db, bs) != j_be32(buf + 0x10)) break;   /* torn commit: log ends */
            }
            seq++; blk = e4_j_next(j, blk);
        } else if (type == 5) {                               /* revoke */
            if (cs && !e4_j_tail_ok(j, buf)) { if (pass == J_SCAN) break; j->why = "revoke block checksum"; return -1; }
            if (pass == J_REVOKE) {
                uint32_t cnt = j_be32(buf + 12), rs = j->b64 ? 8 : 4;
                if (cnt > bs - (cs ? 4 : 0)) { j->why = "bad revoke count"; return -1; }
                for (uint32_t o = 16; o + rs <= cnt; o += rs) {
                    uint64_t rb = j->b64 ? ((uint64_t)j_be32(buf + o) << 32 | j_be32(buf + o + 4)) : j_be32(buf + o);
                    if (e4_j_addrv(j, rb, seq) != 0) { j->why = "out of memory"; return -1; }
                }
            }
            blk = e4_j_next(j, blk);
        } else {
            if (pass == J_SCAN) break;
            j->why = "unknown journal block"; return -1;
        }
    }
    if (pass == J_SCAN) j->end = seq;
    return 0;
}
/* open the journal: 1 = clean (nothing to do), 0 = needs replay, <0 = refuse */
static int e4_j_open(e4_vol_t *v, e4_j_t *j, uint8_t *buf) {
    memset(j, 0, sizeof(*j)); j->v = v;
    uint8_t *sb = (uint8_t *)kalloc(1024);
    if (!sb) { j->why = "out of memory"; return -1; }
    int r = storage_read_sectors(v->dev, v->part + 2, 2, sb);
    uint32_t inum = *(uint32_t *)(sb + 0xE0), jdev = *(uint32_t *)(sb + 0xE4);
    kfree(sb);
    if (r != 0) { j->why = "superblock unreadable"; return -1; }
    if (jdev || !inum) { j->why = "external journal"; return -1; }
    if (e4_iget(v, inum, &j->jin) != 0) { j->why = "journal inode unreadable"; return -1; }
    if (e4_j_read(j, 0, buf, NULL) != 0) { j->why = "journal superblock unreadable"; return -1; }
    uint32_t bt = j_be32(buf + 4);
    if (j_be32(buf) != JBD2_MAGIC || (bt != 3 && bt != 4)) { j->why = "bad journal superblock"; return -1; }
    if (j_be32(buf + 0x0C) != v->bs) { j->why = "journal block size differs"; return -1; }
    j->last = j_be32(buf + 0x10); j->first = j_be32(buf + 0x14); j->seq = j_be32(buf + 0x18); j->start = j_be32(buf + 0x1C);
    j->jsb_errno = j_be32(buf + 0x20);
    if (j->last < 2 || (uint64_t)j->last * v->bs > j->jin.size || j->first < 1 || j->first >= j->last) { j->why = "bad journal geometry"; return -1; }
    if (bt == 4) {
        j->inc = j_be32(buf + 0x28);
        if (j->inc & ~0x1Fu) { j->why = (j->inc & 0x20) ? "fast_commit journal" : "unknown journal features"; return -1; }
        j->c3 = (j->inc & 0x10) != 0; j->c2 = !j->c3 && (j->inc & 0x08); j->b64 = (j->inc & 0x02) != 0;
    }
    if (j->c2 || j->c3) {
        if (buf[0x50] != 4) { j->why = "journal checksum type"; return -1; }
        uint32_t st = j_be32(buf + 0xFC);
        memset(buf + 0xFC, 0, 4);
        if (e4_crc32c(~0u, buf, 1024) != st) { j->why = "journal superblock checksum"; return -1; }
        j_put32(buf + 0xFC, st);
        j->seed = e4_crc32c(~0u, buf + 0x30, 16);
    }
    j->tagsz = j->c3 ? 16 : 8 + (j->b64 ? 4 : 0) + (j->c2 ? 2 : 0);
    if (j->start == 0) return 1;
    if (j->start < j->first || j->start >= j->last) { j->why = "bad log start"; return -1; }
    return 0;
}
/* scan + revoke + verify; 0 ok */
static int e4_j_prepare(e4_j_t *j, uint8_t *buf, uint8_t *db) {
    if (e4_j_pass(j, J_SCAN, buf, db, 0) || e4_j_pass(j, J_REVOKE, buf, db, 0) || e4_j_pass(j, J_VERIFY, buf, db, 0)) return -1;
    return 0;
}
/* rw enable: replay into place, empty the log, clear needs_recovery.  0 ok */
static int e4_j_recover(e4_vol_t *v) {
    e4_j_t *j = (e4_j_t *)kalloc(sizeof(e4_j_t));
    uint8_t *buf = (uint8_t *)kalloc(v->bs), *db = (uint8_t *)kalloc(v->bs);
    int rc = -1;
    if (!j || !buf || !db) goto out;
    int o = e4_j_open(v, j, buf);
    if (o < 0) goto bad;
    if (o == 0) {
        if (e4_j_prepare(j, buf, db) || e4_j_pass(j, J_REPLAY, buf, db, 0)) goto bad;
    }
    /* empty log: journal sb s_start = 0, s_sequence = end + 1 (as e2fsck) */
    if (e4_j_read(j, 0, buf, NULL) != 0) { j->why = "journal superblock unreadable"; goto bad; }
    {
        uint64_t jpb = e4_bmap(v, &j->jin, 0);
        j_put32(buf + 0x1C, 0);
        if (o == 0) j_put32(buf + 0x18, j->end + 1);
        if (j->jsb_errno) j_put32(buf + 0x20, 0);
        if (j->c2 || j->c3) { memset(buf + 0xFC, 0, 4); j_put32(buf + 0xFC, e4_crc32c(~0u, buf, 1024)); }
        if (storage_write_sectors(v->dev, v->part + jpb * v->spb, v->spb, buf) != 0) { j->why = "journal superblock write"; goto bad; }
    }
    {
        uint8_t *sb = (uint8_t *)kalloc(1024);
        if (!sb) goto out;
        if (storage_read_sectors(v->dev, v->part + 2, 2, sb) != 0 || *(uint16_t *)(sb + 56) != 0xEF53) { kfree(sb); j->why = "superblock unreadable after replay"; goto bad; }
        *(uint32_t *)(sb + 0x60) &= ~(uint32_t)I_RECOVER;
        if ((int32_t)j->jsb_errno) *(uint16_t *)(sb + 0x3A) |= 2;            /* journal recorded an error */
        if (*(uint32_t *)(sb + 0x64) & RO_METADATA_CSUM) *(uint32_t *)(sb + 0x3FC) = e4_crc32c(~0u, sb, 0x3FC);
        int w = storage_write_sectors(v->dev, v->part + 2, 2, sb);
        kfree(sb);
        if (w != 0) { j->why = "superblock write"; goto bad; }
    }
    /* group descriptors may have been replayed */
    {
        uint32_t gblocks = (v->groups * v->dsz + v->bs - 1) / v->bs;
        for (uint32_t i = 0; i < gblocks; i++) if (e4_read(v, v->first_data + 1 + i, v->gdt + i * v->bs) != 0) { j->why = "group descriptors unreadable"; goto bad; }
    }
    v->incompat &= ~(uint32_t)I_RECOVER;
    printf("ext4 '%s': journal replayed: %u transaction(s), %u block(s) written, %u revoked\n", v->label, o == 0 ? j->end - j->seq : 0, j->nwritten, j->nrevoked);
    serial_printf("[ext4ro] '%s': journal replayed (seq %u..%u, %u tags, %u written, %u revoked, csum %s%s)\n", v->label, j->seq, j->end, j->ntags, j->nwritten, j->nrevoked,
                  j->c3 ? "v3" : j->c2 ? "v2" : "none", j->b64 ? ", 64bit" : "");
    rc = 0; goto out;
bad:
    printf("ext4 '%s': journal recovery refused: %s\n", v->label, j->why ? j->why : "?");
    serial_printf("[ext4ro] '%s': journal recovery refused: %s\n", v->label, j->why ? j->why : "?");
out:
    if (j && j->rv) kfree(j->rv);
    if (j) kfree(j);
    if (buf) kfree(buf);
    if (db) kfree(db);
    return rc;
}
/* read-only mount of a volume needing recovery: committed blocks become an
 * in-memory overlay (nothing written).  0 ok */
static int e4_j_overlay(e4_vol_t *v) {
    e4_j_t *j = (e4_j_t *)kalloc(sizeof(e4_j_t));
    uint8_t *buf = (uint8_t *)kalloc(v->bs), *db = (uint8_t *)kalloc(v->bs);
    int rc = -1;
    if (!j || !buf || !db) goto out;
    int o = e4_j_open(v, j, buf);
    if (o != 0) { if (o > 0) rc = 0; goto out; }
    if (e4_j_prepare(j, buf, db)) goto out;
    if (j->ntags) {
        v->ovl = (struct e4_ovl *)kalloc(sizeof(struct e4_ovl) * j->ntags);
        if (!v->ovl) { j->why = "out of memory"; goto out; }
        if (e4_j_pass(j, J_OVERLAY, buf, db, j->ntags)) { kfree(v->ovl); v->ovl = NULL; v->novl = 0; goto out; }
        uint32_t gblocks = (v->groups * v->dsz + v->bs - 1) / v->bs;
        for (uint32_t i = 0; i < gblocks; i++) e4_read(v, v->first_data + 1 + i, v->gdt + i * v->bs);
    }
    serial_printf("[ext4ro] '%s': journal needs recovery - %u committed block(s) from %u transaction(s) applied in memory (read-only)\n", v->label, v->novl, j->end - j->seq);
    rc = 0;
out:
    if (rc && j) serial_printf("[ext4ro] '%s': journal not usable (%s) - reading as-is\n", v->label, j->why ? j->why : "?");
    if (j && j->rv) kfree(j->rv);
    if (j) kfree(j);
    if (buf) kfree(buf);
    if (db) kfree(db);
    return rc;
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
    bool csum = (roc & RO_METADATA_CSUM) != 0;
    uint32_t seed = 0;
    if (csum) {
        uint32_t c = e4_crc32c(~0u, sb, 0x3FC), st = *(uint32_t *)(sb + 0x3FC);
        if (sb[0x175] != 1 || c != st) {
            printf("ext4: superblock checksum mismatch (stored %#x computed %#x) - not mounting\n", st, c);
            kfree(sb); return NULL;
        }
        seed = (inc & I_CSUM_SEED) ? *(uint32_t *)(sb + 0x270) : e4_crc32c(~0u, sb + 0x68, 16);
    }
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
    v->incompat = inc; v->ro_compat = roc; v->compat = *(uint32_t *)(sb + 0x5C);
    v->first_ino = rev >= 1 ? *(uint32_t *)(sb + 0x54) : 11; v->want_extra = rev >= 1 ? *(uint16_t *)(sb + 0x15E) : 0;
    v->csum = csum; v->seed = seed;
    memcpy(v->hseed, sb + 0xEC, 16); v->hunsigned = (*(uint32_t *)(sb + 0x160) & 2) ? 3 : 0;
    memcpy(v->label, sb + 0x78, 16); v->label[16] = 0;
    kfree(sb);
    if (v->bs > 65536 || v->bpg == 0 || v->ipg == 0 || v->isz < 128) { printf("ext4: bad geometry\n"); kfree(v); return NULL; }
    v->groups = (uint32_t)((v->blocks - v->first_data + v->bpg - 1) / v->bpg);
    uint32_t gbytes = v->groups * v->dsz, gblocks = (gbytes + v->bs - 1) / v->bs;
    v->gdt = (uint8_t *)kalloc(gblocks * v->bs);
    if (!v->gdt) { kfree(v); return NULL; }
    for (uint32_t i = 0; i < gblocks; i++)
        if (e4_read(v, v->first_data + 1 + i, v->gdt + i * v->bs) != 0) { kfree(v->gdt); kfree(v); return NULL; }
    if (v->csum) {
        for (uint32_t g = 0; g < v->groups; g++) {
            const uint8_t *gd = v->gdt + (uint64_t)g * v->dsz;
            uint32_t c = e4_crc32c(v->seed, &g, 4);
            c = e4_crc32c(c, gd, 0x1E);
            c = e4_crc32c(c, e4_zero4, 2);
            if (v->dsz > 0x20) c = e4_crc32c(c, gd + 0x20, v->dsz - 0x20);
            uint16_t st = *(uint16_t *)(gd + 0x1E);
            if ((c & 0xFFFF) != st) {
                printf("ext4: group %u descriptor checksum mismatch (stored %#x computed %#x) - not mounting\n",
                       g, st, c & 0xFFFF);
                kfree(v->gdt); kfree(v); return NULL;
            }
        }
    }
    if ((inc & I_RECOVER) && (v->compat & 0x0004)) {
        if (e4_j_overlay(v) == 0) printf("ext4: journal needs recovery - committed transactions applied in memory (read-only; replayed when made read-write)\n");
        else printf("ext4: journal needs recovery and cannot be read - reading as-is (read-only)\n");
    } else if (inc & I_RECOVER) printf("ext4: journal needs recovery - reading as-is (read-only)\n");
    serial_printf("[ext4ro] mounted dev %u LBA %u label '%s': %u-byte blocks, %u groups, incompat %#x ro_compat %#x%s\n",
                  dev, lba, v->label, v->bs, v->groups, inc, roc, v->csum ? ", metadata_csum verified" : "");
    return v;
}

extern uint64_t rtc_unix_epoch(void);
/* ---- journal writer: log, commit, checkpoint ---- */
static int e4_free_range(e4_vol_t *v, uint64_t start, uint32_t cnt);
static void e4_orphans_process(e4_vol_t *v);
static uint32_t e4_jw_next(struct e4_jw *w, uint32_t b) { if (++b >= w->last) { w->nwrap++; return w->first; } return b; }
static int e4_sb_update(e4_vol_t *v, bool clean);
static uint32_t e4_jw_space(struct e4_jw *w) {
    uint32_t cap = w->last - w->first;
    if (!w->start) return cap - 1;
    uint32_t used = w->head >= w->start ? w->head - w->start : (w->last - w->start) + (w->head - w->first);
    return used + 1 >= cap ? 0 : cap - used - 1;
}
static int e4_jw_lwrite(e4_vol_t *v, uint32_t jb, const void *buf) {
    return storage_write_sectors(v->dev, v->part + v->jw->jmap[jb] * v->spb, v->spb, buf);
}
static int e4_jw_sb(e4_vol_t *v, uint8_t *buf) {           /* journal superblock: s_sequence, s_start */
    struct e4_jw *w = v->jw;
    if (storage_read_sectors(v->dev, v->part + w->jmap[0] * v->spb, v->spb, buf) != 0 || j_be32(buf) != JBD2_MAGIC) return -1;
    j_put32(buf + 0x18, w->start ? w->tseq : w->seq);
    j_put32(buf + 0x1C, w->start);
    if (w->c2 || w->c3) { memset(buf + 0xFC, 0, 4); j_put32(buf + 0xFC, e4_crc32c(~0u, buf, 1024)); }
    return e4_jw_lwrite(v, 0, buf);
}
static void e4_jw_tail(struct e4_jw *w, uint8_t *b, uint32_t bs) {
    if (!(w->c2 || w->c3)) return;
    memset(b + bs - 4, 0, 4); j_put32(b + bs - 4, e4_crc32c(w->seed, b, bs));
}
/* write every committed block home, then mark the log empty */
static int e4_jw_checkpoint(e4_vol_t *v) {
    struct e4_jw *w = v->jw;
    if (!w || w->depth) return 0;
    int rc = 0;
    for (uint32_t i = 0; i < w->ncb; ) {
        if (w->cb[i].st == E4_CB_DONE) {
            if (storage_write_sectors(v->dev, v->part + w->cb[i].blk * v->spb, v->spb, w->cb[i].buf) != 0) { rc = -1; i++; continue; }
            e4_cb_drop(w, i);
        } else i++;
    }
    if (rc) return rc;
    if (w->start) {
        uint8_t *b = (uint8_t *)kalloc(v->bs);
        if (!b) return -1;
        w->start = 0;
        rc = e4_jw_sb(v, b);
        kfree(b);
        if (rc) return rc;
        w->nckpt++;
    }
    w->nlg = 0;                                      /* the log continues at head (and wraps) */
    return 0;
}
static void e4_tx_begin(e4_vol_t *v) {
    struct e4_jw *w = v->jw;
    if (!w || w->depth++) return;
    w->snap_a = v->alloc_since_sync; w->snap_i = v->ialloc_since_sync;
    if (e4_jw_space(w) < w->reserve || w->ndone >= E4_JW_MAXCB) {
        w->depth = 0;
        if (e4_jw_checkpoint(v) != 0) w->broken = true;
        w->depth = 1; w->nforced++;
    }
}
static int e4_jw_commit(e4_vol_t *v) {
    struct e4_jw *w = v->jw;
    uint32_t nrun = 0, bs = v->bs;
    for (uint32_t i = 0; i < w->ncb; i++) if (w->cb[i].st == E4_CB_RUN) nrun++;
    if (!nrun && !w->nrv) return 0;
    uint32_t cs = (w->c2 || w->c3) ? 4 : 0, rs = w->b64 ? 8 : 4;
    uint32_t per_rv = (bs - 16 - cs) / rs, nrvb = (w->nrv + per_rv - 1) / per_rv;
    uint32_t room = bs - 12 - cs, ndesc = 0, used = room + 1;
    for (uint32_t i = 0; i < nrun; i++) { if (used + w->tagsz > room) { ndesc++; used = w->tagsz + 16; } else used += w->tagsz; }
    uint32_t need = nrvb + ndesc + nrun + 1;
    uint8_t *b = (uint8_t *)kalloc(bs), *d = (uint8_t *)kalloc(bs);
    if (!b || !d) { if (b) kfree(b); if (d) kfree(d); return -1; }
    int rc = 0;
    if (need > e4_jw_space(w) || need > w->txmax || w->broken) {
        /* cannot be logged atomically: abort - drop the running changes (home blocks were never
         * touched), restore the in-memory allocation state; the caller makes the volume read-only */
        serial_printf("[ext4ro] '%s': transaction of %u blocks cannot be journaled (%u free, max %u%s) - aborted\n", v->label, need, e4_jw_space(w), w->txmax, w->broken ? ", journal error" : "");
        for (uint32_t i = 0; i < w->ncb; ) {
            if (w->cb[i].st != E4_CB_RUN) { i++; continue; }
            if (w->cb[i].cbuf) { kfree(w->cb[i].buf); w->cb[i].buf = w->cb[i].cbuf; w->cb[i].cbuf = NULL; w->cb[i].st = E4_CB_DONE; w->ndone++; i++; }
            else e4_cb_drop(w, i);
        }
        for (uint32_t i = 0; i < w->ncb; i++) w->cb[i].fr = 0;
        w->nrv = 0; w->naborted++; w->aborted = true;
        v->alloc_since_sync = w->snap_a; v->ialloc_since_sync = w->snap_i;
        uint32_t gblocks = (v->groups * v->dsz + v->bs - 1) / v->bs;
        for (uint32_t g = 0; g < gblocks; g++) e4_read(v, v->first_data + 1 + g, v->gdt + g * v->bs);
        kfree(b); kfree(d);
        return -1;
    }
    uint32_t pos = w->head, first = pos, seq = w->seq;
    /* revoke blocks */
    for (uint32_t r = 0; r < w->nrv && !rc; ) {
        memset(b, 0, bs); j_put32(b, JBD2_MAGIC); j_put32(b + 4, 5); j_put32(b + 8, seq);
        uint32_t o = 16;
        while (r < w->nrv && o + rs <= bs - cs) {
            if (w->b64) { j_put32(b + o, (uint32_t)(w->rv[r] >> 32)); j_put32(b + o + 4, (uint32_t)w->rv[r]); }
            else j_put32(b + o, (uint32_t)w->rv[r]);
            o += rs; r++;
        }
        j_put32(b + 12, o);
        e4_jw_tail(w, b, bs);
        if (e4_jw_lwrite(v, pos, b) != 0) rc = -1;
        pos = e4_jw_next(w, pos);
    }
    /* descriptor blocks, each followed by its block copies */
    uint32_t i = 0;
    while (i < w->ncb && !rc) {
        memset(b, 0, bs); j_put32(b, JBD2_MAGIC); j_put32(b + 4, 1); j_put32(b + 8, seq);
        uint32_t dpos = pos; pos = e4_jw_next(w, pos);
        uint8_t *p = b + 12, *lastp = NULL; bool firsttag = true;
        while (i < w->ncb && !rc) {
            if (w->cb[i].st != E4_CB_RUN) { i++; continue; }
            uint32_t tl = w->tagsz + (firsttag ? 16 : 0);
            if (p + tl > b + bs - cs) break;
            const uint8_t *src = w->cb[i].buf; uint32_t fl = firsttag ? 0 : 2;
            if (j_be32(src) == JBD2_MAGIC) { memcpy(d, src, bs); memset(d, 0, 4); src = d; fl |= 1; }
            uint8_t sq[4]; j_put32(sq, seq);
            uint32_t c = (w->c2 || w->c3) ? e4_crc32c(e4_crc32c(w->seed, sq, 4), src, bs) : 0;
            j_put32(p, (uint32_t)w->cb[i].blk);
            if (w->c3) { j_put32(p + 4, fl); if (w->b64) j_put32(p + 8, (uint32_t)(w->cb[i].blk >> 32)); j_put32(p + 12, c); }
            else { p[4] = (uint8_t)(c >> 8); p[5] = (uint8_t)c; p[6] = (uint8_t)(fl >> 8); p[7] = (uint8_t)fl; if (w->b64) j_put32(p + 8, (uint32_t)(w->cb[i].blk >> 32)); }
            lastp = p;
            p += w->tagsz;
            if (firsttag) { memcpy(p, w->uuid, 16); p += 16; firsttag = false; }
            if (e4_jw_lwrite(v, pos, src) != 0) rc = -1;
            pos = e4_jw_next(w, pos);
            if (!e4_u64_has(w->lg, w->nlg, w->cb[i].blk) && e4_u64_add(&w->lg, &w->nlg, &w->lgcap, w->cb[i].blk) != 0) rc = -1;
            w->nlogged++;
            i++;
        }
        if (!lastp) { pos = dpos; break; }                 /* nothing left for this descriptor */
        if (w->c3) j_put32(lastp + 4, j_be32(lastp + 4) | 8); else lastp[7] |= 8;
        e4_jw_tail(w, b, bs);
        if (e4_jw_lwrite(v, dpos, b) != 0) rc = -1;
    }
    /* first transaction in an empty log: point the journal superblock at it */
    if (!rc && !w->start) { w->start = first; w->tseq = seq; if (e4_jw_sb(v, b) != 0) rc = -1; }
    if (!rc) {
        memset(b, 0, bs); j_put32(b, JBD2_MAGIC); j_put32(b + 4, 2); j_put32(b + 8, seq);
        uint64_t now = rtc_unix_epoch();
        j_put32(b + 0x30, (uint32_t)(now >> 32)); j_put32(b + 0x34, (uint32_t)now);
        if (w->c2 || w->c3) j_put32(b + 0x10, e4_crc32c(w->seed, b, bs));
        if (e4_jw_lwrite(v, pos, b) != 0) rc = -1;          /* the commit point */
        pos = e4_jw_next(w, pos);
    }
    kfree(b); kfree(d);
    if (rc) { w->broken = true; serial_printf("[ext4ro] '%s': journal commit failed (seq %u)\n", v->label, seq); return rc; }
    for (uint32_t k = 0; k < w->ncb; k++) if (w->cb[k].st == E4_CB_RUN) {
        w->cb[k].st = E4_CB_DONE; w->ndone++;
        if (w->cb[k].cbuf) { kfree(w->cb[k].cbuf); w->cb[k].cbuf = NULL; }
    }
    for (uint32_t k = 0; k < w->ncb; ) { if (w->cb[k].fr) e4_cb_drop(w, k); else k++; }   /* free is committed */
    w->nrv = 0; w->head = pos; w->seq = seq + 1; w->ncommit++;
    return 0;
}
static int e4_tx_end(e4_vol_t *v) {
    struct e4_jw *w = v->jw;
    if (!w || !w->depth) return 0;
    if (w->depth == 1) {
        /* end of the operation: apply the deferred frees, journal the superblock free counts */
        w->applying = true;
        for (uint32_t i = 0; i < w->npf; i++) if (e4_free_range(v, w->pfs[i], (uint32_t)w->pfc[i]) != 0) w->broken = true;
        w->applying = false; w->npf = 0;
        if ((v->alloc_since_sync || v->ialloc_since_sync) && e4_sb_update(v, false) != 0) w->broken = true;
    }
    if (--w->depth) return 0;
    int r = e4_jw_commit(v);
    if (r && w->aborted) {
        /* nothing of this operation is in the log or at home (file data excepted, ordered mode):
         * the committed log stays as it is (needs_recovery remains set, replayed at the next
         * read-write mount or by e2fsck); go read-only */
        v->rw = false;
        printf("ext4 '%s': journal transaction aborted - volume is now read-only\n", v->label);
    }
    return r;
}
/* rw enable on a journaled volume (test-only option): log must be empty */
static int e4_jw_open(e4_vol_t *v, const char **why) {
    e4_j_t *j = (e4_j_t *)kalloc(sizeof(e4_j_t));
    uint8_t *buf = (uint8_t *)kalloc(v->bs);
    struct e4_jw *w = (struct e4_jw *)kalloc(sizeof(struct e4_jw));
    int rc = -1;
    if (!j || !buf || !w) { *why = "out of memory"; goto out; }
    memset(w, 0, sizeof(*w));
    int o = e4_j_open(v, j, buf);
    if (o < 0) { *why = j->why; goto out; }
    if (o != 1) { *why = "journal not empty"; goto out; }
    if ((j_be32(buf + 0x24) & 1) && !(j->c2 || j->c3)) { *why = "journal checksum v1"; goto out; }
    if (j->inc & 0x04) { *why = "async_commit journal"; goto out; }
    w->jin = j->jin; w->first = j->first; w->last = j->last; w->seq = j->seq; w->seed = j->seed; w->tagsz = j->tagsz;
    w->c2 = j->c2; w->c3 = j->c3; w->b64 = j->b64; memcpy(w->uuid, buf + 0x30, 16);
    w->head = w->first;
    w->reserve = (w->last - w->first) / 2;
    if (w->reserve > 1024) w->reserve = 1024;
    w->txmax = w->last - w->first - 2;
    if (v->jres && v->jres < w->reserve) w->reserve = v->jres;
    if (v->jtxmax && v->jtxmax < w->txmax) w->txmax = v->jtxmax;
    w->jmap = (uint64_t *)kalloc(sizeof(uint64_t) * w->last);
    if (!w->jmap) { *why = "out of memory"; goto out; }
    for (uint32_t i = 0; i < w->last; i++) {
        uint64_t pb = e4_bmap(v, &w->jin, i);
        if (!pb || pb == E4_BMAP_ERR || pb >= v->blocks) { *why = "journal inode mapping"; kfree(w->jmap); goto out; }
        w->jmap[i] = pb;
    }
    v->jw = w; w = NULL; rc = 0;
    serial_printf("[ext4ro] '%s': journaled writes ON: log %u blocks, seq %u, csum %s%s\n", v->label, j->last - j->first, j->seq,
                  j->c3 ? "v3" : j->c2 ? "v2" : "none", j->b64 ? ", 64bit" : "");
out:
    if (w) kfree(w);
    if (j) kfree(j);
    if (buf) kfree(buf);
    return rc;
}
static void e4_jw_close(e4_vol_t *v) {
    struct e4_jw *w = v->jw;
    if (!w) return;
    while (w->ncb) e4_cb_drop(w, 0);
    if (w->cb) kfree(w->cb);
    if (w->rv) kfree(w->rv);
    if (w->lg) kfree(w->lg);
    if (w->pfs) kfree(w->pfs);
    if (w->pfc) kfree(w->pfc);
    if (w->jmap) kfree(w->jmap);
    serial_printf("[ext4ro] '%s': journal: %u commits, %u blocks logged, %u revoked, %u checkpoints (%u forced), %u wraps, %u aborted, %u deferred frees, %u orphans%s\n", v->label,
                  w->ncommit, w->nlogged, w->nrevoke, w->nckpt, w->nforced, w->nwrap, w->naborted, w->ndeferred, w->norphan, w->broken ? ", BROKEN" : "");
    kfree(w); v->jw = NULL;
}
void ext4ro_set_journal_test(e4_vol_t *v, bool on);
void ext4ro_set_journal_test(e4_vol_t *v, bool on) { if (v) v->jtest = on; }
/* TEST ONLY: forced-checkpoint reserve and max transaction size, in journal blocks (0 = default) */
void ext4ro_set_journal_limits(e4_vol_t *v, uint32_t reserve, uint32_t txmax);
void ext4ro_set_journal_limits(e4_vol_t *v, uint32_t reserve, uint32_t txmax) { if (v) { v->jres = reserve; v->jtxmax = txmax; } }
static int e4_sync_vol(e4_vol_t *v);
void ext4ro_unmount(e4_vol_t *v) {
    if (!v) return;
    v->closing = true;
    if (v->rw && e4_sync_vol(v) != 0) printf("ext4 '%s': could not mark the volume clean\n", v->label);
    e4_jw_close(v);
    for (int i = 0; i < E4_MAX_VOLS; i++) if (e4_vols[i] == v) e4_vols[i] = NULL;
    if (v->ovl) kfree(v->ovl);
    kfree(v->gdt); kfree(v);
}
const char *ext4ro_label(const e4_vol_t *v) { return v ? v->label : ""; }
void ext4ro_info(const e4_vol_t *v) {
    if (v) printf("ext4 '%s' on device %u at LBA %u: %u-byte blocks, %u groups (%s)\n",
                  v->label, v->dev, v->part, v->bs, v->groups, v->rw ? "read-write" : "read-only");
}

/* ---- read-write (step 1: in-place overwrite of allocated blocks only) ----
 * Allowed only on a clean, journal-less ext2/ext4 whose every feature is one
 * this driver keeps consistent.  Nothing in the bitmaps, group descriptors or
 * superblock is modified: data blocks are rewritten in place and the inode's
 * mtime/ctime (and checksum) are updated. */
#define C_DIR_PREALLOC 0x0001
#define C_IMAGIC       0x0002
#define C_HAS_JOURNAL  0x0004
#define C_EXT_ATTR     0x0008
#define C_RESIZE_INODE 0x0010
#define C_DIR_INDEX    0x0020
#define C_SPARSE_SB2   0x0200
#define C_ORPHAN_FILE  0x1000
#define C_RW_OK   (C_DIR_PREALLOC | C_IMAGIC | C_EXT_ATTR | C_RESIZE_INODE | C_DIR_INDEX | C_SPARSE_SB2 | C_ORPHAN_FILE)
#define I_ENCRYPT  0x10000
#define I_CASEFOLD 0x20000
#define I_RW_OK   (I_FILETYPE | I_EXTENTS | I_64BIT | I_FLEX_BG | I_CSUM_SEED)
#define RO_BIGALLOC 0x0200
#define RO_RW_OK  (0x0001 /*sparse_super*/ | 0x0002 /*large_file*/ | 0x0008 /*huge_file*/ | 0x0010 /*gdt_csum*/ | \
                   0x0020 /*dir_nlink*/ | 0x0040 /*extra_isize*/ | RO_METADATA_CSUM)
extern uint64_t rtc_unix_epoch(void);
/* Rewrite the primary superblock: s_state VALID_FS set (clean) or cleared
 * (mounted read-write), s_wtime, the free-block count (allocations since the
 * last update) and the checksum. */
static int e4_sb_update(e4_vol_t *v, bool clean) {
    uint8_t *sb = (uint8_t *)kalloc(1024);
    if (!sb) return -1;
    int rc = -1;
    bool jt = v->jw && v->jw->depth;
    if (e4_sread(v, v->part + 2, 2, sb) != 0 || *(uint16_t *)(sb + 56) != 0xEF53) goto out;
    if (v->csum && e4_crc32c(~0u, sb, 0x3FC) != *(uint32_t *)(sb + 0x3FC)) { e4_csum_bad(v, "superblock", 0, *(uint32_t *)(sb + 0x3FC), 0); goto out; }
    uint16_t *st = (uint16_t *)(sb + 0x3A);
    if (clean) *st |= 1; else *st &= (uint16_t)~1u;
    if (v->jw) { if (clean) *(uint32_t *)(sb + 0x60) &= ~(uint32_t)I_RECOVER; else *(uint32_t *)(sb + 0x60) |= I_RECOVER; }
    if (v->alloc_since_sync) {
        uint64_t f = *(uint32_t *)(sb + 0x0C) | ((v->incompat & I_64BIT) ? (uint64_t)*(uint32_t *)(sb + 0x158) << 32 : 0);
        int64_t nf = (int64_t)f - v->alloc_since_sync;
        f = nf < 0 ? 0 : (uint64_t)nf;
        *(uint32_t *)(sb + 0x0C) = (uint32_t)f;
        if (v->incompat & I_64BIT) *(uint32_t *)(sb + 0x158) = (uint32_t)(f >> 32);
    }
    if (v->ialloc_since_sync) {
        int64_t fi = (int64_t)*(uint32_t *)(sb + 0x10) - v->ialloc_since_sync;
        *(uint32_t *)(sb + 0x10) = fi < 0 ? 0 : (uint32_t)fi;
    }
    *(uint32_t *)(sb + 0x30) = (uint32_t)rtc_unix_epoch();          /* s_wtime */
    if (v->csum) *(uint32_t *)(sb + 0x3FC) = e4_crc32c(~0u, sb, 0x3FC);
    if ((jt ? e4_swrite(v, v->part + 2, 2, sb) : storage_write_sectors(v->dev, v->part + 2, 2, sb)) != 0) goto out;
    v->alloc_since_sync = 0; v->ialloc_since_sync = 0; v->sb_clean = clean; rc = 0;
out:
    kfree(sb);
    return rc;
}
static int e4_mark_dirty(e4_vol_t *v) { return v->sb_clean ? e4_sb_update(v, false) : 0; }
/* sync / unmount: free count + clean flag. */
static int e4_sync_vol(e4_vol_t *v) {
    if (!v || !v->rw) return 0;
    if (v->jw) {
        if (v->jw->depth) return 0;
        if (e4_jw_commit(v) != 0 || e4_jw_checkpoint(v) != 0) return -1;
        if (!v->closing) return 0;                       /* journaled: clean only at unmount */
    }
    if (v->sb_clean && !v->alloc_since_sync && !v->ialloc_since_sync) return 0;
    return e4_sb_update(v, true);
}

int ext4ro_enable_rw(e4_vol_t *v) {
    if (!v) return -1;
    if (v->rw) return 0;
    uint8_t *sb = (uint8_t *)kalloc(1024);
    if (!sb) return -1;
    const char *why = NULL;
    if (storage_read_sectors(v->dev, v->part + 2, 2, sb) == 0 && (*(uint32_t *)(sb + 0x60) & I_RECOVER) && (*(uint32_t *)(sb + 0x5C) & C_HAS_JOURNAL)) {
        if (v->ovl) { kfree(v->ovl); v->ovl = NULL; v->novl = 0; }
        if (e4_j_recover(v) != 0) why = "journal needs recovery (replay refused)";
    }
    uint32_t bad = 0;
    if (why) ;
    else if (storage_read_sectors(v->dev, v->part + 2, 2, sb) != 0 || *(uint16_t *)(sb + 56) != 0xEF53) why = "superblock unreadable";
    else {
        uint32_t c = *(uint32_t *)(sb + 0x5C), inc = *(uint32_t *)(sb + 0x60), roc = *(uint32_t *)(sb + 0x64);
        uint16_t state = *(uint16_t *)(sb + 0x3A);
        uint32_t rev = *(uint32_t *)(sb + 0x4C);
        if (inc & I_RECOVER) why = "journal needs recovery";
        else if ((!(state & 1) && !(c & C_HAS_JOURNAL)) || (state & 2)) {   /* journaled: unclean is normal after replay */ why = "not cleanly unmounted or has errors - run e2fsck"; bad = state; }
        else if (*(uint32_t *)(sb + 0x194)) why = "error records present (s_error_count) - run e2fsck";
        else if (inc & I_INLINE) why = "inline_data";
        else if (inc & I_META_BG) why = "meta_bg";
        else if (roc & RO_BIGALLOC) why = "bigalloc";
        else if (inc & I_ENCRYPT) why = "encryption";
        else if (inc & I_CASEFOLD) why = "casefold";
        else if (inc & I_MMP) why = "mmp";
        else if (inc & ~I_RW_OK) { why = "unknown/unsupported INCOMPAT features"; bad = inc & ~I_RW_OK; }
        else if (roc & ~RO_RW_OK) { why = "unknown/unsupported RO_COMPAT features"; bad = roc & ~RO_RW_OK; }
        else if (c & ~(C_RW_OK | C_HAS_JOURNAL)) { why = "unknown/unsupported COMPAT features"; bad = c & ~C_RW_OK; }
        else if ((c & C_HAS_JOURNAL) && e4_jw_open(v, &why) != 0) ;
        else if (rev < 1 && v->isz != 128) why = "bad inode size";
        else if (v->csum_errs) why = "metadata checksum errors seen";
    }
    kfree(sb);
    if (why) {
        printf("ext4 '%s': read-write refused: %s%s", v->label, why, bad ? "" : "\n");
        if (bad) printf(" (%#x)\n", bad);
        if (bad) serial_printf("[ext4ro] rw refused '%s': %s (%#x)\n", v->label, why, bad);
        else serial_printf("[ext4ro] rw refused '%s': %s\n", v->label, why);
        return -30;
    }
    v->rw = true; v->sb_clean = true;
    if (e4_sb_update(v, false) != 0) { v->rw = false; e4_jw_close(v); printf("ext4 '%s': cannot write superblock - read-only\n", v->label); return -5; }
    serial_printf("[ext4ro] '%s' read-write enabled (state not-clean until %s)\n", v->label, v->jw ? "unmount" : "sync/unmount");
    if (v->jw) e4_orphans_process(v);
    return 0;
}
bool ext4ro_is_rw(const e4_vol_t *v) { return v && v->rw; }

/* Set mtime = ctime = now and rewrite the inode (with checksum). */
static int e4_touch(e4_vol_t *v, e4_inode_t *in) {
    uint32_t ino = in->ino;
    uint32_t g = (ino - 1) / v->ipg, idx = (ino - 1) % v->ipg;
    uint64_t off = (uint64_t)idx * v->isz;
    uint64_t blk = e4_itable(v, g) + off / v->bs;
    uint32_t sec0 = (uint32_t)((off % v->bs) / 512), nsec = (v->isz + 511) / 512;
    if (v->isz > 1024 || (off % 512) + v->isz > nsec * 512u) return -1;
    uint8_t *b = (uint8_t *)kalloc(nsec * 512);
    if (!b) return -1;
    uint64_t lba = v->part + blk * v->spb + sec0;
    int rc = -1;
    if (e4_sread(v, lba, nsec, b) != 0) goto out;
    uint8_t *r = b + (off % 512);
    if (*(uint16_t *)r != in->mode) goto out;                       /* inode changed under us */
    if (v->csum) {
        uint32_t st, c = e4_inode_csum(v, ino, r, v->isz, NULL, &st);
        if (c != st) { e4_csum_bad(v, "inode (before write)", ino, st, c); goto out; }
    }
    uint64_t now = rtc_unix_epoch();
    memcpy(r + 0x28, in->iblock, 60);                               /* i_block (extents / map) */
    *(uint16_t *)(r + 0x1A) = (uint16_t)in->links;
    *(uint32_t *)(r + 0x04) = (uint32_t)in->size;
    *(uint32_t *)(r + 0x6C) = (uint32_t)(in->size >> 32);
    *(uint32_t *)(r + 0x1C) = *(uint32_t *)(in->raw + 0x1C);        /* i_blocks */
    *(uint16_t *)(r + 0x74) = *(uint16_t *)(in->raw + 0x74);
    *(uint32_t *)(r + 0x0C) = (uint32_t)now;                        /* ctime */
    *(uint32_t *)(r + 0x10) = (uint32_t)now;                        /* mtime */
    uint16_t extra = v->isz > 128 ? *(uint16_t *)(r + 0x80) : 0;
    uint32_t ehi = (uint32_t)((now >> 32) & 3);                     /* epoch bits, nsec 0 */
    if (128u + extra >= 0x88) *(uint32_t *)(r + 0x84) = ehi;
    if (128u + extra >= 0x8C) *(uint32_t *)(r + 0x88) = ehi;
    if (v->csum) {
        uint32_t c = e4_inode_csum(v, ino, r, v->isz, NULL, NULL);
        *(uint16_t *)(r + 0x7C) = (uint16_t)c;
        if (v->isz > 128 && 128u + extra >= 0x84) *(uint16_t *)(r + 0x82) = (uint16_t)(c >> 16);
    }
    if (blk < v->first_data + 1 || blk >= v->blocks) goto out;
    if (e4_swrite(v, lba, nsec, b) != 0) goto out;
    in->mtime = (uint32_t)now;
    memcpy(in->raw, r, in->rawlen);
    rc = 0;
out:
    kfree(b);
    return rc;
}

/* ---- block allocation ---- */
#define BG_BLOCK_UNINIT 0x0002
static uint16_t e4_gd_csum(e4_vol_t *v, uint32_t g) {
    const uint8_t *gd = v->gdt + (uint64_t)g * v->dsz;
    uint32_t c = e4_crc32c(v->seed, &g, 4);
    c = e4_crc32c(c, gd, 0x1E);
    c = e4_crc32c(c, e4_zero4, 2);
    if (v->dsz > 0x20) c = e4_crc32c(c, gd + 0x20, v->dsz - 0x20);
    return (uint16_t)c;
}
static uint64_t e4_gd_bitmap(e4_vol_t *v, uint32_t g) {
    const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    uint64_t t = *(uint32_t *)(d + 0);
    if (v->dsz >= 64) t |= (uint64_t)*(uint32_t *)(d + 0x20) << 32;
    return t;
}
static uint32_t e4_gd_free(e4_vol_t *v, uint32_t g) {
    const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    return *(uint16_t *)(d + 0x0C) | (v->dsz >= 64 ? (uint32_t)*(uint16_t *)(d + 0x2C) << 16 : 0);
}
/* Bitmap + descriptor update for one block (+1 alloc / -1 free). */
static int e4_bitmap_commit(e4_vol_t *v, uint32_t g, uint8_t *bm, int delta) {
    uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    if (v->csum) {
        uint32_t c = e4_crc32c(v->seed, bm, v->bpg / 8);
        *(uint16_t *)(d + 0x18) = (uint16_t)c;
        if (v->dsz >= 0x3C) *(uint16_t *)(d + 0x38) = (uint16_t)(c >> 16);
    }
    uint32_t f = e4_gd_free(v, g) - (uint32_t)delta;
    *(uint16_t *)(d + 0x0C) = (uint16_t)f;
    if (v->dsz >= 64) *(uint16_t *)(d + 0x2C) = (uint16_t)(f >> 16);
    if (v->csum) *(uint16_t *)(d + 0x1E) = e4_gd_csum(v, g);
    if (e4_swrite(v, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) != 0) return -1;
    uint64_t gblk = v->first_data + 1 + ((uint64_t)g * v->dsz) / v->bs;
    uint32_t goff = (uint32_t)(((uint64_t)g * v->dsz) / v->bs) * v->bs;
    if (e4_swrite(v, v->part + gblk * v->spb, v->spb, v->gdt + goff) != 0) return -1;
    v->alloc_since_sync += delta;
    return 0;
}
/* Allocate one block, first free at/after @goal (wrapping).  0 = none. */
static bool e4_has_super(e4_vol_t *v, uint32_t g) {
    if (g <= 1 || !(v->ro_compat & 0x0001)) return true;
    for (uint32_t b = 3; b <= 7; b += 2) { uint64_t p = b; while (p < g) p *= b; if (p == g) return true; }
    return false;
}
static void e4_bm_mark(uint8_t *bm, uint64_t gstart, uint32_t nbits, uint64_t a, uint64_t n) {
    for (uint64_t x = a; x < a + n; x++) if (x >= gstart && x < gstart + nbits) bm[(x - gstart) >> 3] |= (uint8_t)(1u << ((x - gstart) & 7));
}
/* 0 = @bm now holds the bitmap of BLOCK_UNINIT group @g (matches its free count) */
static int e4_bm_uninit(e4_vol_t *v, uint32_t g, uint8_t *bm) {
    if (v->compat & 0x0200) return -1;                               /* sparse_super2: not handled */
    uint64_t gstart = v->first_data + (uint64_t)g * v->bpg;
    uint32_t nbits = v->bpg;
    if (gstart + nbits > v->blocks) nbits = (uint32_t)(v->blocks - gstart);
    memset(bm, 0, v->bs);
    if (e4_has_super(v, g)) {
        uint8_t *sb = (uint8_t *)kalloc(1024);
        if (!sb) return -1;
        int r = storage_read_sectors(v->dev, v->part + 2, 2, sb);
        uint32_t rsv = *(uint16_t *)(sb + 0xCE);
        kfree(sb);
        if (r != 0) return -1;
        uint32_t gdtb = (uint32_t)(((uint64_t)v->groups * v->dsz + v->bs - 1) / v->bs);
        e4_bm_mark(bm, gstart, nbits, gstart, 1 + gdtb + rsv);
    }
    const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    uint64_t it = *(uint32_t *)(d + 8) | (v->dsz >= 64 ? (uint64_t)*(uint32_t *)(d + 0x28) << 32 : 0);
    e4_bm_mark(bm, gstart, nbits, e4_gd_bitmap(v, g), 1);
    e4_bm_mark(bm, gstart, nbits, *(uint32_t *)(d + 4) | (v->dsz >= 64 ? (uint64_t)*(uint32_t *)(d + 0x24) << 32 : 0), 1);
    e4_bm_mark(bm, gstart, nbits, it, ((uint64_t)v->ipg * v->isz + v->bs - 1) / v->bs);
    uint32_t used = 0;
    for (uint32_t i = 0; i < nbits; i++) if (bm[i >> 3] & (1u << (i & 7))) used++;
    for (uint32_t i = nbits; i < v->bpg; i++) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (used + e4_gd_free(v, g) != nbits) {
        serial_printf("[ext4ro] group %u: BLOCK_UNINIT bitmap would not match the free count (%u used, %u free) - skipped\n", g, used, e4_gd_free(v, g));
        return -1;
    }
    return 0;
}
static uint64_t e4_alloc_block(e4_vol_t *v, uint64_t goal) {
    if ((v->ro_compat & 0x0010) && !v->csum) return 0;          /* gdt_csum (crc16) not implemented */
    if (goal < v->first_data || goal >= v->blocks) goal = v->first_data;
    uint32_t g0 = (uint32_t)((goal - v->first_data) / v->bpg);
    uint8_t *bm = (uint8_t *)kalloc(v->bs);
    if (!bm) return 0;
    uint64_t res = 0;
    for (uint32_t k = 0; k <= v->groups && !res; k++) {
        uint32_t g = (g0 + k) % v->groups;
        const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
        if (!e4_gd_free(v, g)) continue;
        bool uninit = (*(uint16_t *)(d + 0x12) & BG_BLOCK_UNINIT) != 0;
        if (uninit) {
            if (e4_bm_uninit(v, g, bm) != 0) continue;
            *(uint16_t *)((uint8_t *)d + 0x12) &= (uint16_t)~BG_BLOCK_UNINIT;  /* committed with the first allocation */
        } else if (e4_sread(v, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) != 0) break;
        if (v->csum && !uninit) {
            uint32_t c = e4_crc32c(v->seed, bm, v->bpg / 8);
            uint32_t st = *(uint16_t *)(d + 0x18) | (v->dsz >= 0x3C ? (uint32_t)*(uint16_t *)(d + 0x38) << 16 : 0);
            if ((v->dsz >= 0x3C ? c : (c & 0xFFFF)) != st) { e4_csum_bad(v, "block bitmap", g, st, c); break; }
        }
        uint64_t gstart = v->first_data + (uint64_t)g * v->bpg;
        uint32_t nbits = v->bpg;
        if (gstart + nbits > v->blocks) nbits = (uint32_t)(v->blocks - gstart);
        uint32_t start = (k == 0) ? (uint32_t)(goal - gstart) : 0;
        for (int pass = 0; pass < 2 && !res; pass++) {
            uint32_t a = pass ? 0 : start, z = pass ? start : nbits;
            for (uint32_t i = a; i < z; i++) {
                if (bm[i >> 3] & (1u << (i & 7))) continue;
                bm[i >> 3] |= (uint8_t)(1u << (i & 7));
                if (e4_bitmap_commit(v, g, bm, 1) != 0) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); goto done; }
                res = gstart + i; break;
            }
            if (k != 0) break;
        }
    }
done:
    kfree(bm);
    return res;
}
static void e4_free_block(e4_vol_t *v, uint64_t pb) {
    if (e4_jw_defer(v, pb, 1)) return;
    uint32_t g = (uint32_t)((pb - v->first_data) / v->bpg), i = (uint32_t)((pb - v->first_data) % v->bpg);
    uint8_t *bm = (uint8_t *)kalloc(v->bs);
    if (!bm) return;
    if (e4_sread(v, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) == 0 &&
        (bm[i >> 3] & (1u << (i & 7)))) {
        bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
        e4_bitmap_commit(v, g, bm, -1);
    }
    kfree(bm);
}
static void e4_iblocks_add(e4_vol_t *v, e4_inode_t *in, int nblk) {
    uint8_t *r = in->raw;
    uint64_t b = *(uint32_t *)(r + 0x1C) | ((v->ro_compat & RO_HUGE_FILE) ? (uint64_t)*(uint16_t *)(r + 0x74) << 32 : 0);
    b += (in->flags & FL_HUGE_FILE) ? (uint64_t)nblk : (uint64_t)nblk * (v->bs / 512);
    *(uint32_t *)(r + 0x1C) = (uint32_t)b;
    if (v->ro_compat & RO_HUGE_FILE) *(uint16_t *)(r + 0x74) = (uint16_t)(b >> 32);
}

/* ---- mapping growth ---- */
#define E4_NO_APPEND 0xFFFFFFFFu
static void e4_leaf_csum(e4_vol_t *v, const e4_inode_t *in, uint8_t *blk) {
    if (!v->csum) return;
    uint32_t to = 12 + 12u * *(uint16_t *)(blk + 4);
    *(uint32_t *)(blk + to) = e4_crc32c(in->cseed, blk, to);
}
/* End of the mapping (first logical block after the last mapped one) and
 * the physical block after it (allocation goal).  E4_NO_APPEND = can't. */
static uint32_t e4_map_end(e4_vol_t *v, const e4_inode_t *in, uint64_t *goal) {
    *goal = 0;
    if (!(in->flags & FL_EXTENTS)) {
        uint32_t e = (uint32_t)((in->size + v->bs - 1) / v->bs);
        for (uint32_t k = e; k > 0 && k + 64 > e; k--) {
            uint64_t pb = e4_bmap(v, in, k - 1);
            if (pb && pb != E4_BMAP_ERR) { *goal = pb + 1 + (e - k); break; }
        }
        return e;
    }
    const uint8_t *node = in->iblock;
    uint8_t *buf = NULL;
    uint32_t res = E4_NO_APPEND;
    for (int guard = 0; guard < 6; guard++) {
        if (*(uint16_t *)node != 0xF30A) break;
        uint16_t n = *(uint16_t *)(node + 2), depth = *(uint16_t *)(node + 6);
        if (depth == 0) {
            if (n == 0) { res = 0; break; }
            const uint8_t *e = node + 12 + 12 * (n - 1);
            uint32_t lb = *(uint32_t *)e; uint16_t len = *(uint16_t *)(e + 4);
            if (len > 32768) len -= 32768;
            uint64_t st = *(uint32_t *)(e + 8) | ((uint64_t)*(uint16_t *)(e + 6) << 32);
            res = lb + len; *goal = st + len;
            break;
        }
        if (n == 0) break;
        const uint8_t *ix = node + 12 + 12 * (n - 1);
        uint64_t leaf = *(uint32_t *)(ix + 4) | ((uint64_t)*(uint16_t *)(ix + 8) << 32);
        if (!buf && !(buf = (uint8_t *)kalloc(v->bs))) break;
        if (e4_read(v, leaf, buf) != 0) break;
        node = buf;
    }
    if (buf) kfree(buf);
    return res;
}

/* ---- block maps (ext2/3, no extents): set lblk -> pb, allocating indirect blocks ---- */
static int e4_ind_set(e4_vol_t *v, e4_inode_t *in, uint32_t lblk, uint64_t pb) {
    uint32_t *ib = (uint32_t *)in->iblock, per = v->bs / 4;
    if (pb >> 32) return -27;
    if (lblk < 12) { ib[lblk] = (uint32_t)pb; return 0; }
    uint64_t rel = lblk - 12; uint32_t lvl;
    if (rel < per) lvl = 1;
    else if ((rel -= per) < (uint64_t)per * per) lvl = 2;
    else { rel -= (uint64_t)per * per; lvl = 3; if (rel >= (uint64_t)per * per * per) return -27; }
    uint32_t *buf = (uint32_t *)kalloc(v->bs);
    if (!buf) return -12;
    int rc = 0;
    uint32_t *slot = &ib[11 + lvl];
    if (!*slot) {
        uint64_t nb = e4_alloc_block(v, pb);
        if (!nb) { kfree(buf); return -28; }
        memset(buf, 0, v->bs);
        if (e4_wblk(v, nb, buf) != 0) { e4_free_block(v, nb); kfree(buf); return -5; }
        *slot = (uint32_t)nb; e4_iblocks_add(v, in, 1);
    }
    uint32_t cur = *slot;
    for (uint32_t l = lvl; l > 0; l--) {
        uint64_t div = 1;
        for (uint32_t k = 1; k < l; k++) div *= per;
        uint32_t idx = (uint32_t)((rel / div) % per);
        if (e4_read(v, cur, buf) != 0) { rc = -5; break; }
        if (l == 1) { buf[idx] = (uint32_t)pb; rc = e4_wblk(v, cur, buf) ? -5 : 0; break; }
        if (!buf[idx]) {
            uint64_t nb = e4_alloc_block(v, pb);
            if (!nb) { rc = -28; break; }
            uint32_t *z = (uint32_t *)kalloc(v->bs);
            if (!z) { e4_free_block(v, nb); rc = -12; break; }
            memset(z, 0, v->bs);
            int w = e4_wblk(v, nb, z); kfree(z);
            if (w) { e4_free_block(v, nb); rc = -5; break; }
            buf[idx] = (uint32_t)nb;
            if (e4_wblk(v, cur, buf) != 0) { rc = -5; break; }
            e4_iblocks_add(v, in, 1);
        }
        cur = buf[idx];
    }
    kfree(buf);
    return rc;
}
/* Add lblk->pb (lblk >= map end): merge into the last extent, add an entry,
 * or (root leaf full) move the root's entries into a new leaf block. */
static int e4_leaf_add(uint8_t *node, uint32_t lblk, uint64_t pb) {
    uint16_t n = *(uint16_t *)(node + 2), max = *(uint16_t *)(node + 4);
    if (n) {
        uint8_t *e = node + 12 + 12 * (n - 1);
        uint32_t lb = *(uint32_t *)e; uint16_t len = *(uint16_t *)(e + 4);
        uint64_t st = *(uint32_t *)(e + 8) | ((uint64_t)*(uint16_t *)(e + 6) << 32);
        if (len < 32768 && lb + len == lblk && st + len == pb) { *(uint16_t *)(e + 4) = len + 1; return 0; }
    }
    if (n >= max) return 1;                                          /* full */
    uint8_t *e = node + 12 + 12 * n;
    *(uint32_t *)e = lblk; *(uint16_t *)(e + 4) = 1;
    *(uint16_t *)(e + 6) = (uint16_t)(pb >> 32); *(uint32_t *)(e + 8) = (uint32_t)pb;
    *(uint16_t *)(node + 2) = n + 1;
    return 0;
}
static int e4_ext_set(e4_vol_t *v, e4_inode_t *in, uint32_t lblk, uint64_t pb);
static int e4_map_append(e4_vol_t *v, e4_inode_t *in, uint32_t lblk, uint64_t pb) {
    if (!(in->flags & FL_EXTENTS)) return e4_ind_set(v, in, lblk, pb);
    return e4_ext_set(v, in, lblk, pb);
}


/* ---- extents: map lblk -> pb inside the mapping (fill a hole, or mark one
 * block of an unwritten extent written), with splits/merges, root -> leaf and
 * leaf splits under a depth-1 root (deeper trees: not yet) ---- */
typedef struct { uint32_t lb, len; uint64_t pb; bool unw; } e4_x_t;
static int e4_x_load(const uint8_t *node, e4_x_t *x) {
    uint16_t n = *(uint16_t *)(node + 2);
    for (int i = 0; i < n; i++) {
        const uint8_t *e = node + 12 + 12 * i;
        uint16_t raw = *(uint16_t *)(e + 4);
        x[i].lb = *(uint32_t *)e; x[i].unw = raw > 32768; x[i].len = x[i].unw ? raw - 32768u : raw;
        x[i].pb = *(uint32_t *)(e + 8) | ((uint64_t)*(uint16_t *)(e + 6) << 32);
    }
    return n;
}
static void e4_x_store(uint8_t *node, const e4_x_t *x, int n) {
    uint16_t max = *(uint16_t *)(node + 4);
    memset(node + 12, 0, 12u * max);
    for (int i = 0; i < n; i++) {
        uint8_t *e = node + 12 + 12 * i;
        *(uint32_t *)e = x[i].lb; *(uint16_t *)(e + 4) = (uint16_t)(x[i].unw ? x[i].len + 32768u : x[i].len);
        *(uint16_t *)(e + 6) = (uint16_t)(x[i].pb >> 32); *(uint32_t *)(e + 8) = (uint32_t)x[i].pb;
    }
    *(uint16_t *)(node + 2) = (uint16_t)n;
}
/* apply lblk -> pb to the sorted list (room for n + 2); returns the new count */
static int e4_x_apply(e4_x_t *x, int n, uint32_t lblk, uint64_t pb) {
    int i = 0;
    while (i < n && x[i].lb + x[i].len <= lblk) i++;
    if (i < n && x[i].lb <= lblk) {                                  /* inside an (unwritten) extent */
        e4_x_t o = x[i], p[3]; int k = 0;
        if (lblk > o.lb) { p[k].lb = o.lb; p[k].len = lblk - o.lb; p[k].pb = o.pb; p[k].unw = o.unw; k++; }
        p[k].lb = lblk; p[k].len = 1; p[k].pb = pb; p[k].unw = false; k++;
        if (lblk + 1 < o.lb + o.len) { p[k].lb = lblk + 1; p[k].len = o.lb + o.len - lblk - 1; p[k].pb = o.pb + (lblk + 1 - o.lb); p[k].unw = o.unw; k++; }
        memmove(x + i + k, x + i + 1, sizeof(e4_x_t) * (n - i - 1));
        memcpy(x + i, p, sizeof(e4_x_t) * k);
        n += k - 1;
    } else {                                                          /* hole: new entry */
        memmove(x + i + 1, x + i, sizeof(e4_x_t) * (n - i));
        x[i].lb = lblk; x[i].len = 1; x[i].pb = pb; x[i].unw = false;
        n++;
    }
    /* merge written neighbours that are contiguous logically and physically */
    for (int k = 1; k < n; ) {
        e4_x_t *a = &x[k - 1], *b = &x[k];
        if (!a->unw && !b->unw && a->lb + a->len == b->lb && a->pb + a->len == b->pb && a->len + b->len <= 32768) {
            a->len += b->len; memmove(x + k, x + k + 1, sizeof(e4_x_t) * (n - k - 1)); n--;
        } else k++;
    }
    return n;
}
static bool e4_leaf_csum_ok(e4_vol_t *v, const e4_inode_t *in, const uint8_t *blk, uint64_t at) {
    if (*(uint16_t *)blk != 0xF30A) return false;
    if (!v->csum) return true;
    uint32_t to = 12 + 12u * *(uint16_t *)(blk + 4);
    if (to + 4 > v->bs || e4_crc32c(in->cseed, blk, to) != *(uint32_t *)(blk + to)) { e4_csum_bad(v, "extent block", at, 0, 0); return false; }
    return true;
}
#define E4_MAXD 5                                   /* ext4 limit */
typedef struct { uint8_t *buf; uint64_t blk; int idx; } e4_pl_t;   /* path level; 0 = root in the inode */
typedef struct { uint64_t b[E4_MAXD + 2]; int n, used; } e4_pool_t;
static uint64_t e4_pool_take(e4_pool_t *p) { return p->used < p->n ? p->b[p->used++] : 0; }
static void e4_pool_release(e4_vol_t *v, e4_pool_t *p) { while (p->n > p->used) e4_free_block(v, p->b[--p->n]); }
static bool e4_node_ok(e4_vol_t *v, const e4_inode_t *in, const uint8_t *b, uint64_t at, int depth) {
    if (*(uint16_t *)b != 0xF30A || *(uint16_t *)(b + 6) != depth) return false;
    uint16_t max = *(uint16_t *)(b + 4);
    if (!max || max > (v->bs - 12) / 12 || *(uint16_t *)(b + 2) > max) return false;
    return e4_leaf_csum_ok(v, in, b, at);
}
static int e4_pl_write(e4_vol_t *v, e4_inode_t *in, e4_pl_t *pl, int L) {
    if (L == 0) return 0;                          /* root: written with the inode (e4_touch) */
    e4_leaf_csum(v, in, pl[L].buf);
    return e4_wblk(v, pl[L].blk, pl[L].buf) ? -5 : 0;
}
static void e4_ix_put(uint8_t *e, uint32_t key, uint64_t child) {
    memset(e, 0, 12); *(uint32_t *)e = key; *(uint32_t *)(e + 4) = (uint32_t)child; *(uint16_t *)(e + 8) = (uint16_t)(child >> 32);
}
/* insert index entry {key, child} at @pos of index node @L; splits full nodes
 * upward and grows the root depth when the root is full */
static int e4_ix_insert(e4_vol_t *v, e4_inode_t *in, e4_pl_t *pl, int L, int pos, uint32_t key, uint64_t child, e4_pool_t *pool) {
    uint8_t *nd = pl[L].buf;
    uint16_t n = *(uint16_t *)(nd + 2), max = *(uint16_t *)(nd + 4), d = *(uint16_t *)(nd + 6);
    uint16_t lmax = (uint16_t)((v->bs - 12) / 12);
    if (n < max) {
        memmove(nd + 12 + 12 * (pos + 1), nd + 12 + 12 * pos, 12u * (n - pos));
        e4_ix_put(nd + 12 + 12 * pos, key, child); *(uint16_t *)(nd + 2) = n + 1;
        return e4_pl_write(v, in, pl, L);
    }
    uint64_t nb = e4_pool_take(pool);
    if (!nb) return -28;
    uint8_t *b = (uint8_t *)kalloc(v->bs), *t = (uint8_t *)kalloc(12u * (n + 1));
    if (!b || !t) { if (b) kfree(b); if (t) kfree(t); return -12; }
    memcpy(t, nd + 12, 12u * pos); e4_ix_put(t + 12 * pos, key, child); memcpy(t + 12 * (pos + 1), nd + 12 + 12 * pos, 12u * (n - pos));
    int rc;
    memset(b, 0, v->bs); *(uint16_t *)b = 0xF30A; *(uint16_t *)(b + 6) = d;
    if (L == 0) {                                  /* root full: entries move down, depth + 1 */
        *(uint16_t *)(b + 2) = n + 1; *(uint16_t *)(b + 4) = lmax; memcpy(b + 12, t, 12u * (n + 1));
        e4_leaf_csum(v, in, b);
        rc = e4_wblk(v, nb, b) ? -5 : 0;
        if (!rc) {
            memset(nd + 12, 0, 48); *(uint16_t *)(nd + 2) = 1; *(uint16_t *)(nd + 6) = d + 1;
            e4_ix_put(nd + 12, *(uint32_t *)t, nb);
            e4_iblocks_add(v, in, 1);
            serial_printf("[ext4ro] ino %u: extent tree depth %u -> %u (index block %llu)\n", in->ino, d, d + 1, (unsigned long long)nb);
        }
    } else {                                       /* split this index node */
        int left = pos == n ? n : (n + 1) / 2;
        memset(nd + 12, 0, 12u * max); memcpy(nd + 12, t, 12u * left); *(uint16_t *)(nd + 2) = (uint16_t)left;
        *(uint16_t *)(b + 2) = (uint16_t)(n + 1 - left); *(uint16_t *)(b + 4) = max; memcpy(b + 12, t + 12 * left, 12u * (n + 1 - left));
        uint32_t rkey = *(uint32_t *)(b + 12);
        e4_leaf_csum(v, in, b);
        rc = e4_wblk(v, nb, b) ? -5 : e4_pl_write(v, in, pl, L);
        if (!rc) {
            e4_iblocks_add(v, in, 1);
            serial_printf("[ext4ro] ino %u: extent index split at depth %u -> block %llu\n", in->ino, d, (unsigned long long)nb);
            rc = e4_ix_insert(v, in, pl, L - 1, pl[L - 1].idx + 1, rkey, nb, pool);
        }
    }
    kfree(b); kfree(t);
    return rc;
}
/* map lblk -> pb anywhere: fill a hole, append, or mark one block of an
 * unwritten extent written; splits/merges extents, splits leaves and index
 * nodes, grows the tree depth (up to 5) */
static int e4_ext_set(e4_vol_t *v, e4_inode_t *in, uint32_t lblk, uint64_t pb) {
    uint8_t *root = in->iblock;
    if (*(uint16_t *)root != 0xF30A) {                                /* empty file without a header */
        memset(root, 0, 60); *(uint16_t *)root = 0xF30A; *(uint16_t *)(root + 4) = 4;
    }
    int D = *(uint16_t *)(root + 6);
    if (D > E4_MAXD || *(uint16_t *)(root + 2) > 4) return -5;
    uint32_t lmax = (v->bs - 12) / 12;
    e4_pl_t pl[E4_MAXD + 1]; memset(pl, 0, sizeof(pl));
    e4_pool_t pool; memset(&pool, 0, sizeof(pool));
    pl[0].buf = root;
    e4_x_t *x = (e4_x_t *)kalloc(sizeof(e4_x_t) * (lmax + 4));
    uint8_t *nb2 = (uint8_t *)kalloc(v->bs);
    int rc = -12;
    if (!x || !nb2) goto out;
    for (int L = 0; L < D; L++) {
        uint8_t *nd = pl[L].buf; uint16_t n = *(uint16_t *)(nd + 2);
        if (!n) { rc = -5; goto out; }
        int pick = 0;
        for (int i = 0; i < n; i++) if (*(uint32_t *)(nd + 12 + 12 * i) <= lblk) pick = i;
        pl[L].idx = pick;
        uint8_t *ix = nd + 12 + 12 * pick;
        pl[L + 1].blk = *(uint32_t *)(ix + 4) | ((uint64_t)*(uint16_t *)(ix + 8) << 32);
        if (!(pl[L + 1].buf = (uint8_t *)kalloc(v->bs))) goto out;
        if (pl[L + 1].blk < v->first_data + 1 || pl[L + 1].blk >= v->blocks || e4_read(v, pl[L + 1].blk, pl[L + 1].buf) != 0 ||
            !e4_node_ok(v, in, pl[L + 1].buf, pl[L + 1].blk, D - L - 1)) { rc = -5; goto out; }
    }
    uint8_t *leaf = pl[D].buf;
    uint16_t max = *(uint16_t *)(leaf + 4);
    if (max > lmax) { rc = -5; goto out; }
    int n0 = e4_x_load(leaf, x);
    bool tail = n0 == 0 || lblk >= x[n0 - 1].lb;
    int n = e4_x_apply(x, n0, lblk, pb);
    if (n > max) {                                 /* reserve every block a split chain can need */
        int need = 1;
        for (int L = D - 1; L >= 0; L--) { if (*(uint16_t *)(pl[L].buf + 2) >= *(uint16_t *)(pl[L].buf + 4)) need++; else break; }
        uint64_t goal = D ? pl[D].blk + 1 : pb + 1;
        for (int i = 0; i < need; i++) {
            uint64_t b = e4_alloc_block(v, goal);
            if (!b) { e4_pool_release(v, &pool); rc = -28; goto out; }
            pool.b[pool.n++] = b; goal = b + 1;
        }
    }
    /* keys on the path must not exceed the first block below them */
    for (int L = D - 1; L >= 0; L--) {
        uint8_t *e = pl[L].buf + 12 + 12 * pl[L].idx;
        if (*(uint32_t *)e <= x[0].lb) break;
        *(uint32_t *)e = x[0].lb;
        if ((rc = e4_pl_write(v, in, pl, L)) != 0) goto rel;
    }
    if (n <= max) { e4_x_store(leaf, x, n); rc = e4_pl_write(v, in, pl, D); goto rel; }
    uint64_t nb = e4_pool_take(&pool);
    memset(nb2, 0, v->bs);
    *(uint16_t *)nb2 = 0xF30A;
    if (D == 0) {                                  /* root leaf full: entries -> new leaf, depth 1 */
        *(uint16_t *)(nb2 + 4) = (uint16_t)lmax;
        e4_x_store(nb2, x, n); e4_leaf_csum(v, in, nb2);
        if (e4_wblk(v, nb, nb2) != 0) { rc = -5; goto rel; }
        memset(root + 12, 0, 48);
        *(uint16_t *)(root + 2) = 1; *(uint16_t *)(root + 6) = 1;
        e4_ix_put(root + 12, x[0].lb, nb);
        e4_iblocks_add(v, in, 1);
        serial_printf("[ext4ro] ino %u: extent root full - split into leaf block %llu\n", in->ino, (unsigned long long)nb);
        rc = 0; goto rel;
    }
    int left = tail ? (n - 1 < max ? n - 1 : max) : n / 2;  /* appends keep the left leaf full */
    *(uint16_t *)(nb2 + 4) = max;
    e4_x_store(nb2, x + left, n - left); e4_leaf_csum(v, in, nb2);
    e4_x_store(leaf, x, left);
    if (e4_wblk(v, nb, nb2) != 0 || (rc = e4_pl_write(v, in, pl, D)) != 0) { rc = -5; goto rel; }
    e4_iblocks_add(v, in, 1);
    serial_printf("[ext4ro] ino %u: extent leaf split -> block %llu\n", in->ino, (unsigned long long)nb);
    rc = e4_ix_insert(v, in, pl, D - 1, pl[D - 1].idx + 1, x[left].lb, nb, &pool);
rel:
    e4_pool_release(v, &pool);
out:
    for (int L = 1; L <= E4_MAXD; L++) if (pl[L].buf) kfree(pl[L].buf);
    if (x) kfree(x);
    if (nb2) kfree(nb2);
    return rc;
}
/* Write file bytes [off, off+len).  Allocated blocks are rewritten in place;
 * blocks past the end of the mapping are allocated (extents: merged into the
 * last extent / new entries / root split; block maps: direct blocks only).
 * Holes inside the mapping and unwritten extents are not filled yet. */
static int e4_pwrite(e4_vol_t *v, e4_inode_t *in, uint64_t off, const uint8_t *src, uint32_t len) {
    if (!v->rw) return -30;
    if (in->flags & FL_INLINE) return -95;
    if ((in->mode & S_IFMT_) != S_IFREG_) return -22;
    if (len == 0) return 0;
    uint64_t maxsz = (v->ro_compat & 0x0002) ? (uint64_t)0xFFFFFFFFu * v->bs : 0x7FFFFFFFull;
    if (off >= maxsz) return -27;
    if (len > maxsz - off) len = (uint32_t)(maxsz - off);
    if (e4_mark_dirty(v) != 0) return -5;
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -12;
    uint32_t done = 0; int err = 0; bool meta = false;
    /* growing past a partial last block: its tail must read back as zeros */
    if (off + len > in->size && (in->size % v->bs) && off > in->size) {
        uint64_t pb = e4_bmap(v, in, (uint32_t)(in->size / v->bs));
        if (pb && pb != E4_BMAP_ERR && e4_read(v, pb, b) == 0) {
            memset(b + in->size % v->bs, 0, v->bs - in->size % v->bs);
            if (e4_wdata(v, pb, b) != 0) { kfree(b); return -5; }
        }
    }
    while (done < len) {
        uint64_t p = off + done;
        uint32_t lb = (uint32_t)(p / v->bs), in_off = (uint32_t)(p % v->bs);
        uint32_t n = v->bs - in_off; if (n > len - done) n = len - done;
        bool unw = false;
        uint64_t pb = e4_bmap_x(v, in, lb, &unw);
        if (pb == E4_BMAP_ERR) { err = -5; break; }
        if (unw) {                                                    /* unwritten: rest of the block reads as zeros */
            if (pb < v->first_data + 1 || pb >= v->blocks) { err = -5; break; }
            memset(b, 0, v->bs);
            memcpy(b + in_off, src + done, n);
            if (e4_wdata(v, pb, b) != 0) { err = -5; break; }
            int r = e4_ext_set(v, in, lb, pb);
            if (r) { serial_printf("[ext4ro] ino %u lblk %u: unwritten -> written failed (%d)\n", in->ino, lb, r); err = r; break; }
            meta = true;
        } else if (!pb) {
            uint64_t goal;
            uint32_t end = e4_map_end(v, in, &goal);
            if (end == E4_NO_APPEND || lb < end) {                    /* hole inside the mapping */
                uint64_t prev = lb ? e4_bmap(v, in, lb - 1) : 0;
                if (prev == E4_BMAP_ERR) prev = 0;
                pb = e4_alloc_block(v, prev ? prev + 1 : goal);
                if (!pb) { err = -28; break; }
                memset(b, 0, v->bs);
                memcpy(b + in_off, src + done, n);
                if (e4_wdata(v, pb, b) != 0) { e4_free_block(v, pb); err = -5; break; }
                int r = (in->flags & FL_EXTENTS) ? e4_ext_set(v, in, lb, pb) : e4_ind_set(v, in, lb, pb);
                if (r) { e4_free_block(v, pb); serial_printf("[ext4ro] ino %u lblk %u: hole fill failed (%d)\n", in->ino, lb, r); err = r; break; }
                e4_iblocks_add(v, in, 1);
                meta = true;
                done += n;
                if (off + done > in->size) { in->size = off + done; }
                continue;
            }
            if (lb > end) goal += lb - end;
            pb = e4_alloc_block(v, goal);
            if (!pb) { err = -28; break; }
            memset(b, 0, v->bs);
            memcpy(b + in_off, src + done, n);
            if (e4_wdata(v, pb, b) != 0) { e4_free_block(v, pb); err = -5; break; }
            int r = e4_map_append(v, in, lb, pb);
            if (r != 0) {
                e4_free_block(v, pb);
                serial_printf("[ext4ro] ino %u lblk %u: mapping growth failed (%d)\n", in->ino, lb, r);
                err = r; break;
            }
            e4_iblocks_add(v, in, 1);
            meta = true;
        } else if (pb < v->first_data + 1 || pb >= v->blocks) { err = -5; break; }
        else if (in_off == 0 && n == v->bs) {
            if (e4_wdata(v, pb, src + done) != 0) { err = -5; break; }
        } else {
            if (e4_read(v, pb, b) != 0) { err = -5; break; }
            memcpy(b + in_off, src + done, n);
            if (e4_wdata(v, pb, b) != 0) { err = -5; break; }
        }
        done += n;
        if (off + done > in->size) { in->size = off + done; meta = true; }
    }
    kfree(b);
    if ((done || meta) && e4_touch(v, in) != 0) {
        serial_printf("[ext4ro] ino %u: inode update failed\n", in->ino);
        return -5;
    }
    return done ? (int)done : err;
}

/* ---- step 3: truncate, create, unlink ---- */
static int e4_gdt_write(e4_vol_t *v, uint32_t g) {
    uint64_t gblk = v->first_data + 1 + ((uint64_t)g * v->dsz) / v->bs;
    uint32_t goff = (uint32_t)(((uint64_t)g * v->dsz) / v->bs) * v->bs;
    return e4_swrite(v, v->part + gblk * v->spb, v->spb, v->gdt + goff);
}
/* Free [start, start+cnt) in the block bitmaps (only bits that are set). */
static int e4_free_range(e4_vol_t *v, uint64_t start, uint32_t cnt) {
    if (e4_jw_defer(v, start, cnt)) return 0;
    uint8_t *bm = (uint8_t *)kalloc(v->bs);
    if (!bm) return -12;
    int rc = 0;
    while (cnt) {
        if (start < v->first_data + 1 || start >= v->blocks) { rc = -5; break; }
        uint32_t g = (uint32_t)((start - v->first_data) / v->bpg), i = (uint32_t)((start - v->first_data) % v->bpg);
        uint32_t n = v->bpg - i; if (n > cnt) n = cnt;
        if (e4_sread(v, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) != 0) { rc = -5; break; }
        int k = 0;
        for (uint32_t j = i; j < i + n; j++) if (bm[j >> 3] & (1u << (j & 7))) { bm[j >> 3] &= (uint8_t)~(1u << (j & 7)); k++; }
        if (k && e4_bitmap_commit(v, g, bm, -k) != 0) { rc = -5; break; }
        start += n; cnt -= n;
    }
    kfree(bm);
    return rc;
}
/* Drop extents at/after logical block @keep from one leaf node. */
static int e4_leaf_trim(e4_vol_t *v, uint8_t *node, uint32_t keep, uint32_t *freed) {
    uint16_t n = *(uint16_t *)(node + 2), w = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t *e = node + 12 + 12 * i;
        uint32_t lb = *(uint32_t *)e; uint16_t raw = *(uint16_t *)(e + 4);
        bool unw = raw > 32768; uint32_t len = unw ? raw - 32768u : raw;
        uint64_t st = *(uint32_t *)(e + 8) | ((uint64_t)*(uint16_t *)(e + 6) << 32);
        if (lb >= keep) { if (e4_free_range(v, st, len)) return -5; *freed += len; continue; }
        if (lb + len > keep) {
            uint32_t nl = keep - lb;
            if (e4_free_range(v, st + nl, len - nl)) return -5;
            *freed += len - nl;
            *(uint16_t *)(e + 4) = (uint16_t)(unw ? nl + 32768u : nl);
        }
        if (w != i) memmove(node + 12 + 12 * w, e, 12);
        w++;
    }
    for (uint16_t i = w; i < n; i++) memset(node + 12 + 12 * i, 0, 12);
    *(uint16_t *)(node + 2) = w;
    return 0;
}
static int e4_free_ind(e4_vol_t *v, uint32_t blk, int lvl, uint32_t *freed) {
    if (!blk) return 0;
    uint32_t *b = (uint32_t *)kalloc(v->bs);
    if (!b) return -12;
    if (e4_read(v, blk, b) != 0) { kfree(b); return -5; }
    for (uint32_t i = 0; i < v->bs / 4; i++) {
        if (!b[i]) continue;
        if (lvl > 1) { if (e4_free_ind(v, b[i], lvl - 1, freed)) { kfree(b); return -5; } }
        else { if (e4_free_range(v, b[i], 1)) { kfree(b); return -5; } (*freed)++; }
    }
    kfree(b);
    if (e4_free_range(v, blk, 1)) return -5;
    (*freed)++;
    return 0;
}
/* Free every block mapped at logical >= keep (data + emptied tree blocks). */

/* free mapped blocks >= @keep below indirect block @blk (level @lvl, first
 * logical block @base).  1 = @blk is now empty (caller frees it), 0, <0 error */
static int e4_ind_trim(e4_vol_t *v, uint32_t blk, int lvl, uint64_t base, uint32_t keep, uint32_t *freed) {
    uint32_t per = v->bs / 4;
    uint64_t span = 1;
    for (int k = 1; k < lvl; k++) span *= per;
    uint32_t *b = (uint32_t *)kalloc(v->bs);
    if (!b) return -12;
    if (e4_read(v, blk, b) != 0) { kfree(b); return -5; }
    bool any = false, changed = false;
    for (uint32_t i = 0; i < per; i++) {
        if (!b[i]) continue;
        uint64_t lo = base + i * span;
        if (lo + span <= keep) { any = true; continue; }
        if (lvl == 1) { if (e4_free_range(v, b[i], 1)) { kfree(b); return -5; } (*freed)++; b[i] = 0; changed = true; continue; }
        if (lo >= keep) { if (e4_free_ind(v, b[i], lvl - 1, freed)) { kfree(b); return -5; } b[i] = 0; changed = true; continue; }
        int r = e4_ind_trim(v, b[i], lvl - 1, lo, keep, freed);
        if (r < 0) { kfree(b); return r; }
        if (r == 1) { if (e4_free_range(v, b[i], 1)) { kfree(b); return -5; } (*freed)++; b[i] = 0; changed = true; }
        else any = true;
    }
    int rc = 0;
    if (!any) rc = 1;
    else if (changed && e4_wblk(v, blk, b) != 0) rc = -5;
    kfree(b);
    return rc;
}

/* free everything mapped at >= keep below node @nd; empty children (leaf or
 * index blocks) are freed and their index entries dropped */
static int e4_ext_trim(e4_vol_t *v, e4_inode_t *in, uint8_t *nd, uint32_t keep, uint32_t *freed, int lvl) {
    uint16_t d = *(uint16_t *)(nd + 6);
    if (d == 0) return e4_leaf_trim(v, nd, keep, freed);
    if (lvl >= E4_MAXD) return -5;
    uint16_t n = *(uint16_t *)(nd + 2), w = 0;
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -12;
    int rc = 0;
    for (uint16_t i = 0; i < n && !rc; i++) {
        uint8_t *e = nd + 12 + 12 * i;
        uint64_t next = i + 1 < n ? *(uint32_t *)(e + 12) : 0x100000000ull;
        if (next <= keep) { if (w != i) memmove(nd + 12 + 12 * w, e, 12); w++; continue; }
        uint64_t c = *(uint32_t *)(e + 4) | ((uint64_t)*(uint16_t *)(e + 8) << 32);
        if (c < v->first_data + 1 || c >= v->blocks || e4_read(v, c, b) != 0 || !e4_node_ok(v, in, b, c, d - 1)) { rc = -5; break; }
        if ((rc = e4_ext_trim(v, in, b, keep, freed, lvl + 1)) != 0) break;
        if (*(uint16_t *)(b + 2) == 0) {
            if (e4_free_range(v, c, 1)) { rc = -5; break; }
            (*freed)++; continue;                      /* drop the entry */
        }
        e4_leaf_csum(v, in, b);
        if (e4_wblk(v, c, b) != 0) { rc = -5; break; }
        if (w != i) memmove(nd + 12 + 12 * w, e, 12);
        w++;
    }
    kfree(b);
    if (rc) return rc;
    for (uint16_t i = w; i < n; i++) memset(nd + 12 + 12 * i, 0, 12);
    *(uint16_t *)(nd + 2) = w;
    return 0;
}
/* after a truncate: empty root -> depth 0; a single child that fits in the
 * root (<= 4 entries) is pulled up into it */
static int e4_ext_collapse(e4_vol_t *v, e4_inode_t *in, uint32_t *freed) {
    uint8_t *root = in->iblock;
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -12;
    int rc = 0;
    for (int g = 0; g < E4_MAXD && *(uint16_t *)(root + 6) > 0; g++) {
        uint16_t n = *(uint16_t *)(root + 2), d = *(uint16_t *)(root + 6);
        if (n == 0) { *(uint16_t *)(root + 6) = 0; *(uint16_t *)(root + 4) = 4; break; }
        if (n != 1) break;
        uint64_t c = *(uint32_t *)(root + 16) | ((uint64_t)*(uint16_t *)(root + 20) << 32);
        if (c < v->first_data + 1 || c >= v->blocks || e4_read(v, c, b) != 0 || !e4_node_ok(v, in, b, c, d - 1)) { rc = -5; break; }
        uint16_t cn = *(uint16_t *)(b + 2);
        if (cn > 4) break;
        memset(root + 12, 0, 48); memcpy(root + 12, b + 12, 12u * cn);
        *(uint16_t *)(root + 2) = cn; *(uint16_t *)(root + 6) = d - 1;
        if (e4_free_range(v, c, 1)) { rc = -5; break; }
        (*freed)++;
    }
    kfree(b);
    return rc;
}
static int e4_trunc_blocks(e4_vol_t *v, e4_inode_t *in, uint32_t keep) {
    uint32_t freed = 0;
    int rc = 0;
    if (in->flags & FL_EXTENTS) {
        uint8_t *root = in->iblock;
        if (*(uint16_t *)root != 0xF30A)                    /* empty file without an extent header */
            return (*(uint32_t *)(in->raw + 0x1C) == 0 && in->size == 0) ? 0 : -5;
        rc = e4_ext_trim(v, in, root, keep, &freed, 0);
        if (!rc) rc = e4_ext_collapse(v, in, &freed);
    } else {
        uint32_t *ib = (uint32_t *)in->iblock, per = v->bs / 4;
        for (uint32_t i = keep; i < 12; i++) if (ib[i]) { if (e4_free_range(v, ib[i], 1)) return -5; ib[i] = 0; freed++; }
        uint64_t base = 12, span = per;
        for (int l = 0; l < 3 && !rc; l++, base += span, span *= per) {
            if (!ib[12 + l]) continue;
            if (keep <= base) { rc = e4_free_ind(v, ib[12 + l], l + 1, &freed); if (!rc) ib[12 + l] = 0; }
            else if (keep < base + span) {
                rc = e4_ind_trim(v, ib[12 + l], l + 1, base, keep, &freed);
                if (rc == 1) { rc = e4_free_range(v, ib[12 + l], 1) ? -5 : 0; freed++; ib[12 + l] = 0; }
            }
        }
    }
    if (freed) e4_iblocks_add(v, in, -(int)freed);
    return rc;
}
static int e4_trunc(e4_vol_t *v, e4_inode_t *in, uint64_t len) {
    if (!v->rw) return -30;
    if (in->flags & FL_INLINE) return -95;
    if ((in->mode & S_IFMT_) != S_IFREG_) return -22;
    uint64_t maxsz = (v->ro_compat & 0x0002) ? (uint64_t)0xFFFFFFFFu * v->bs : 0x7FFFFFFFull;
    if (len > maxsz) return -27;
    if (e4_mark_dirty(v) != 0) return -5;
    if (len < in->size) {
        int r = e4_trunc_blocks(v, in, (uint32_t)((len + v->bs - 1) / v->bs));
        if (r) { e4_touch(v, in); return r; }
    }
    /* bytes past the new (shrink) or old (grow) end in that block must read as zeros later */
    uint64_t z = len < in->size ? len : in->size;
    if (z % v->bs && len != in->size) {
        uint64_t pb = e4_bmap(v, in, (uint32_t)(z / v->bs));
        uint8_t *b = (uint8_t *)kalloc(v->bs);
        if (b && pb && pb != E4_BMAP_ERR && e4_read(v, pb, b) == 0) {
            memset(b + z % v->bs, 0, v->bs - z % v->bs);
            e4_wdata(v, pb, b);
        }
        if (b) kfree(b);
    }
    in->size = len;
    return e4_touch(v, in) ? -5 : 0;
}

/* inode allocation */
static uint32_t e4_gd_ifree(e4_vol_t *v, uint32_t g) {
    const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    return *(uint16_t *)(d + 0x0E) | (v->dsz >= 64 ? (uint32_t)*(uint16_t *)(d + 0x2E) << 16 : 0);
}
static uint64_t e4_gd_ibitmap(e4_vol_t *v, uint32_t g) {
    const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    return *(uint32_t *)(d + 4) | (v->dsz >= 64 ? (uint64_t)*(uint32_t *)(d + 0x24) << 32 : 0);
}
static int e4_ibitmap_commit(e4_vol_t *v, uint32_t g, uint8_t *bm, int delta, uint32_t idx) {
    uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    if (v->csum) {
        uint32_t c = e4_crc32c(v->seed, bm, v->ipg / 8);
        *(uint16_t *)(d + 0x1A) = (uint16_t)c;
        if (v->dsz >= 0x3C) *(uint16_t *)(d + 0x3A) = (uint16_t)(c >> 16);
    }
    uint32_t f = e4_gd_ifree(v, g) - (uint32_t)delta;
    *(uint16_t *)(d + 0x0E) = (uint16_t)f;
    if (v->dsz >= 64) *(uint16_t *)(d + 0x2E) = (uint16_t)(f >> 16);
    if (delta > 0 && (v->ro_compat & (0x0010 | RO_METADATA_CSUM))) {
        uint32_t un = *(uint16_t *)(d + 0x1C) | (v->dsz >= 64 ? (uint32_t)*(uint16_t *)(d + 0x32) << 16 : 0);
        if (idx >= v->ipg - un) {
            un = v->ipg - idx - 1;
            *(uint16_t *)(d + 0x1C) = (uint16_t)un;
            if (v->dsz >= 64) *(uint16_t *)(d + 0x32) = (uint16_t)(un >> 16);
        }
    }
    if (v->csum) *(uint16_t *)(d + 0x1E) = e4_gd_csum(v, g);
    if (e4_swrite(v, v->part + e4_gd_ibitmap(v, g) * v->spb, v->spb, bm) != 0) return -1;
    if (e4_gdt_write(v, g) != 0) return -1;
    v->ialloc_since_sync += delta;
    return 0;
}
static uint32_t e4_ialloc(e4_vol_t *v, uint32_t g0) {
    if ((v->ro_compat & 0x0010) && !v->csum) return 0;
    uint8_t *bm = (uint8_t *)kalloc(v->bs);
    if (!bm) return 0;
    uint32_t res = 0;
    for (uint32_t k = 0; k < v->groups && !res; k++) {
        uint32_t g = (g0 + k) % v->groups;
        const uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
        if (!e4_gd_ifree(v, g)) continue;
        if (*(uint16_t *)(d + 0x12) & 0x0001 /*INODE_UNINIT*/) {
            if (!v->csum && !(v->ro_compat & 0x0010)) continue;
            memset(bm, 0, v->bs);
            for (uint32_t i = v->ipg; i < v->bs * 8; i++) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
            *(uint16_t *)(v->gdt + (uint64_t)g * v->dsz + 0x12) &= (uint16_t)~0x0001u;   /* commit recomputes the gd csum */
            serial_printf("[ext4ro] group %u: initialising inode bitmap (INODE_UNINIT)\n", g);
        } else if (e4_sread(v, v->part + e4_gd_ibitmap(v, g) * v->spb, v->spb, bm) != 0) break;
        else if (v->csum) {
            uint32_t c = e4_crc32c(v->seed, bm, v->ipg / 8);
            uint32_t st = *(uint16_t *)(d + 0x1A) | (v->dsz >= 0x3C ? (uint32_t)*(uint16_t *)(d + 0x3A) << 16 : 0);
            if ((v->dsz >= 0x3C ? c : (c & 0xFFFF)) != st) { e4_csum_bad(v, "inode bitmap", g, st, c); break; }
        }
        for (uint32_t i = 0; i < v->ipg; i++) {
            uint32_t ino = g * v->ipg + i + 1;
            if (ino < v->first_ino || (bm[i >> 3] & (1u << (i & 7)))) continue;
            bm[i >> 3] |= (uint8_t)(1u << (i & 7));
            if (e4_ibitmap_commit(v, g, bm, 1, i) != 0) goto done;
            res = ino; break;
        }
    }
done:
    kfree(bm);
    return res;
}
static void e4_ifree(e4_vol_t *v, uint32_t ino) {
    uint32_t g = (ino - 1) / v->ipg, i = (ino - 1) % v->ipg;
    uint8_t *bm = (uint8_t *)kalloc(v->bs);
    if (!bm) return;
    if (e4_sread(v, v->part + e4_gd_ibitmap(v, g) * v->spb, v->spb, bm) == 0 && (bm[i >> 3] & (1u << (i & 7)))) {
        bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
        e4_ibitmap_commit(v, g, bm, -1, i);
    }
    kfree(bm);
}
/* Write a whole on-disk inode (checksum computed here). */
static int e4_iwrite(e4_vol_t *v, uint32_t ino, uint8_t *raw) {
    uint32_t g = (ino - 1) / v->ipg, idx = (ino - 1) % v->ipg;
    uint64_t off = (uint64_t)idx * v->isz;
    uint64_t blk = e4_itable(v, g) + off / v->bs;
    uint32_t sec0 = (uint32_t)((off % v->bs) / 512), nsec = (v->isz + 511) / 512;
    if (v->isz > 1024 || (off % 512) + v->isz > nsec * 512u || blk < v->first_data + 1 || blk >= v->blocks) return -1;
    if (v->csum) {
        uint16_t extra = v->isz > 128 ? *(uint16_t *)(raw + 0x80) : 0;
        uint32_t c = e4_inode_csum(v, ino, raw, v->isz, NULL, NULL);
        *(uint16_t *)(raw + 0x7C) = (uint16_t)c;
        if (v->isz > 128 && 128u + extra >= 0x84) *(uint16_t *)(raw + 0x82) = (uint16_t)(c >> 16);
    }
    uint8_t *b = (uint8_t *)kalloc(nsec * 512);
    if (!b) return -1;
    uint64_t lba = v->part + blk * v->spb + sec0;
    int rc = -1;
    if (e4_sread(v, lba, nsec, b) == 0) {
        memcpy(b + off % 512, raw, v->isz);
        rc = e4_swrite(v, lba, nsec, b) == 0 ? 0 : -1;
    }
    kfree(b);
    return rc;
}
/* "a/b/c" -> dir "a/b", name "c" */
static int e4_split(const char *rel, char *dir, int cap, const char **name) {
    int l = (int)strlen(rel ? rel : "");
    while (l > 0 && rel[l - 1] == '/') l--;
    int s = l;
    while (s > 0 && rel[s - 1] != '/') s--;
    if (l - s <= 0 || l - s > 255 || s >= cap) return -22;
    memcpy(dir, rel, s); dir[s] = 0;
    *name = rel + s;
    return l - s;
}
static bool e4_has_tail(e4_vol_t *v, const uint8_t *b) {
    const uint8_t *t = b + v->bs - 12;
    return *(uint32_t *)t == 0 && *(uint16_t *)(t + 4) == 12 && t[6] == 0 && t[7] == 0xDE;
}
static void e4_dir_csum(e4_vol_t *v, const e4_inode_t *d, uint8_t *b) {
    if (v->csum && e4_has_tail(v, b)) *(uint32_t *)(b + v->bs - 4) = e4_crc32c(d->cseed, b, v->bs - 12);
}
static uint32_t e4_dir_lookup(e4_vol_t *v, const e4_inode_t *d, const char *nm, int nl);

/* ---- htree hashed insert ---- */
/* insert into one leaf block if there is room; 1 inserted, 0 full, <0 corrupt */
static int e4_leaf_ins(e4_vol_t *v, uint8_t *b, const char *nm, int nl, uint32_t ino, uint8_t ft) {
    uint32_t need = (8u + nl + 3) & ~3u, limit = v->csum ? v->bs - 12 : v->bs;
    for (uint32_t o = 0; o < limit; ) {
        uint32_t ei = *(uint32_t *)(b + o); uint16_t rl = *(uint16_t *)(b + o + 4); uint8_t enl = b[o + 6];
        if (rl < 8 || o + rl > v->bs) return -5;
        uint32_t act = ei ? ((8u + enl + 3) & ~3u) : 0;
        if (o + rl <= limit && rl >= act + need) {
            uint8_t *e = b + o;
            if (ei) { *(uint16_t *)(e + 4) = (uint16_t)act; e += act; *(uint16_t *)(e + 4) = (uint16_t)(rl - act); }
            *(uint32_t *)e = ino; e[6] = (uint8_t)nl; e[7] = ft; memcpy(e + 8, nm, nl);
            return 1;
        }
        o += rl;
    }
    return 0;
}
/* append one block to directory @d (logical block d->size/bs) */
static int e4_dir_grow(e4_vol_t *v, e4_inode_t *d, uint32_t *lbo, uint64_t *pbo) {
    uint32_t nblk = (uint32_t)(d->size / v->bs);
    uint64_t goal; uint32_t end = e4_map_end(v, d, &goal);
    if (end == E4_NO_APPEND || end != nblk) return -95;
    uint64_t pb = e4_alloc_block(v, goal);
    if (!pb) return -28;
    int r = e4_map_append(v, d, nblk, pb);
    if (r) { e4_free_block(v, pb); return r; }
    e4_iblocks_add(v, d, 1);
    d->size += v->bs;
    *lbo = nblk; *pbo = pb;
    return 0;
}
typedef struct { uint32_t hash; uint16_t off, size; } e4_dxm_t;
/* rebuild leaf @out from entries m[a..b) of leaf @src */
static void e4_leaf_build(e4_vol_t *v, const e4_inode_t *d, uint8_t *out, const uint8_t *src, const e4_dxm_t *m, int a, int b) {
    uint32_t limit = v->csum ? v->bs - 12 : v->bs, o = 0, last = 0;
    memset(out, 0, v->bs);
    for (int i = a; i < b; i++) {
        memcpy(out + o, src + m[i].off, 8 + src[m[i].off + 6]);
        *(uint16_t *)(out + o + 4) = m[i].size; last = o; o += m[i].size;
    }
    *(uint16_t *)(out + last + 4) = (uint16_t)(limit - last);
    if (v->csum) { uint8_t *t = out + v->bs - 12; *(uint16_t *)(t + 4) = 12; t[7] = 0xDE; }
}
static void e4_dx_insert_at(uint8_t *b, uint32_t co, uint32_t pos, uint32_t hash, uint32_t blk) {
    uint32_t cnt = E4_DXCNT(b, co);
    memmove(b + co + 8 * (pos + 1), b + co + 8 * pos, 8 * (cnt - pos));
    *(uint32_t *)(b + co + 8 * pos) = hash; *(uint32_t *)(b + co + 8 * pos + 4) = blk;
    E4_DXCNT(b, co) = (uint16_t)(cnt + 1);
}
static int e4_dx_add(e4_vol_t *v, e4_inode_t *d, const char *nm, int nl, uint32_t ino, uint8_t ft) {
    uint32_t bs = v->bs;
    uint8_t *rb = (uint8_t *)kalloc(bs), *nb = (uint8_t *)kalloc(bs), *lb = (uint8_t *)kalloc(bs);
    uint8_t *xb = (uint8_t *)kalloc(bs), *yb = (uint8_t *)kalloc(bs), *ob = (uint8_t *)kalloc(bs);
    e4_dxm_t *m = (e4_dxm_t *)kalloc(sizeof(e4_dxm_t) * (bs / 12 + 2));
    int rc = -12;
    uint64_t rpb = 0, npb = 0, lpb = 0;
    if (!rb || !nb || !lb || !xb || !yb || !ob || !m) goto out;
    if ((rc = e4_dx_load(v, d, 0, true, rb, &rpb)) != 0) goto out;
    uint32_t lv = rb[30], h;
    if (lv > 1) { serial_printf("[ext4ro] htree insert: %u index levels not supported (ino %u)\n", lv + 1, d->ino); rc = -95; goto out; }
    if ((rc = e4_dxhash(v, rb[28], nm, nl, &h)) != 0) { serial_printf("[ext4ro] htree insert: hash version %u not supported\n", rb[28]); goto out; }
    uint32_t ri = e4_dx_pick(rb, 32, h), nlb = 0, ni;
    if (lv) { nlb = E4_DXB(rb, 32, ri); if ((rc = e4_dx_load(v, d, nlb, false, nb, &npb)) != 0) goto out; ni = e4_dx_pick(nb, 8, h); }
    else ni = ri;
    uint8_t *ib = lv ? nb : rb; uint32_t ico = lv ? 8 : 32;
    uint32_t llb = E4_DXB(ib, ico, ni);
    lpb = llb < d->size / bs ? e4_bmap(v, d, llb) : E4_BMAP_ERR;
    if (lpb == E4_BMAP_ERR || !lpb || e4_read(v, lpb, lb) != 0) { rc = -5; goto out; }
    if (v->csum && !e4_dirblk_ok(v, d, llb, lpb, lb)) { rc = -5; goto out; }
    if ((rc = e4_leaf_ins(v, lb, nm, nl, ino, ft)) != 0) {
        if (rc < 0) goto out;
        e4_dir_csum(v, d, lb);
        rc = e4_wblk(v, lpb, lb) ? -5 : 0;
        if (!rc) rc = e4_touch(v, d) ? -5 : 0;
        goto out;
    }
    /* leaf full: split it.  Will the index have room? */
    int grow = 0, nsplit = 0;
    if (E4_DXCNT(ib, ico) >= E4_DXLIM(ib, ico)) {
        if (!lv) grow = 1;                                          /* root full: add a level */
        else if (E4_DXCNT(rb, 32) < E4_DXLIM(rb, 32)) nsplit = 1;  /* node full: split it */
        else { serial_printf("[ext4ro] htree insert: index full (2 levels) in ino %u\n", d->ino); rc = -28; goto out; }
    }
    /* collect + sort the leaf by hash */
    int cnt = 0;
    for (uint32_t o = 0; o + 8 <= bs; ) {
        uint32_t ei = *(uint32_t *)(lb + o); uint16_t rl = *(uint16_t *)(lb + o + 4); uint8_t enl = lb[o + 6];
        if (rl < 8 || o + rl > bs) { rc = -5; goto out; }
        if (ei && enl) {
            if (cnt >= (int)(bs / 12 + 1) || (rc = e4_dxhash(v, rb[28], (const char *)lb + o + 8, enl, &m[cnt].hash)) != 0) { if (!rc) rc = -5; goto out; }
            m[cnt].off = (uint16_t)o; m[cnt].size = (uint16_t)((8u + enl + 3) & ~3u); cnt++;
        }
        o += rl;
    }
    if (cnt < 2) { rc = -28; goto out; }
    for (int i = 1; i < cnt; i++) { e4_dxm_t t = m[i]; int j = i - 1; while (j >= 0 && m[j].hash > t.hash) { m[j + 1] = m[j]; j--; } m[j + 1] = t; }
    uint32_t sz = 0; int move = 0;
    for (int i = cnt - 1; i >= 0; i--) { if (sz + m[i].size / 2 > bs / 2) break; sz += m[i].size; move++; }
    int split = cnt - move;
    if (split < 1) split = 1;
    if (split >= cnt) split = cnt - 1;
    { int s0 = split; while (split < cnt && m[split].hash == m[split - 1].hash) split++;
      if (split >= cnt) { split = s0; while (split > 0 && m[split].hash == m[split - 1].hash) split--; }
      if (split <= 0) { serial_printf("[ext4ro] htree insert: leaf of equal hashes\n"); rc = -28; goto out; } }
    uint32_t h2 = m[split].hash;
    /* the new entry must fit in the half it belongs to */
    e4_leaf_build(v, d, ob, lb, m, 0, split);
    e4_leaf_build(v, d, xb, lb, m, split, cnt);
    if (e4_leaf_ins(v, h >= h2 ? xb : ob, nm, nl, ino, ft) != 1) { rc = -28; goto out; }
    if (e4_mark_dirty(v) != 0) { rc = -5; goto out; }
    uint32_t xlb, ylb = 0; uint64_t xpb, ypb = 0;
    if ((rc = e4_dir_grow(v, d, &xlb, &xpb)) != 0) goto out;
    if ((grow || nsplit) && (rc = e4_dir_grow(v, d, &ylb, &ypb)) != 0) {      /* undo the leaf block */
        if (e4_trunc_blocks(v, d, xlb) == 0) { d->size = (uint64_t)xlb * bs; e4_touch(v, d); }
        goto out;
    }
    /* index update */
    uint8_t *tb = ib; uint32_t tco = ico, tpos = ni + 1;
    if (grow) {
        memset(yb, 0, bs); *(uint16_t *)(yb + 4) = (uint16_t)bs;
        memcpy(yb + 8, rb + 32, 8 * E4_DXCNT(rb, 32));
        E4_DXLIM(yb, 8) = (uint16_t)e4_dx_limit(v, false); E4_DXCNT(yb, 8) = E4_DXCNT(rb, 32);
        E4_DXCNT(rb, 32) = 1; *(uint32_t *)(rb + 32 + 4) = ylb; rb[30] = 1;
        tb = yb; tco = 8;
    } else if (nsplit) {
        uint32_t nc = E4_DXCNT(nb, 8), half = nc / 2;
        memset(yb, 0, bs); *(uint16_t *)(yb + 4) = (uint16_t)bs;
        uint32_t hsp = E4_DXH(nb, 8, half);
        memcpy(yb + 8, nb + 8 + 8 * half, 8 * (nc - half));
        E4_DXLIM(yb, 8) = (uint16_t)e4_dx_limit(v, false); E4_DXCNT(yb, 8) = (uint16_t)(nc - half);
        E4_DXCNT(nb, 8) = (uint16_t)half;
        e4_dx_insert_at(rb, 32, ri + 1, hsp, ylb);
        if (ni >= half) { tb = yb; tpos = ni - half + 1; }
    }
    e4_dx_insert_at(tb, tco, tpos, h2, xlb);
    e4_dir_csum(v, d, ob); e4_dir_csum(v, d, xb);
    if (grow || nsplit) e4_dx_csum(v, d, yb, 8);
    if (lv) e4_dx_csum(v, d, nb, 8);
    e4_dx_csum(v, d, rb, 32);
    rc = 0;
    if (e4_wblk(v, xpb, xb) || e4_wblk(v, lpb, ob)) rc = -5;
    if (!rc && (grow || nsplit) && e4_wblk(v, ypb, yb)) rc = -5;
    if (!rc && lv && e4_wblk(v, npb, nb)) rc = -5;
    if (!rc && e4_wblk(v, rpb, rb)) rc = -5;
    if (!rc) rc = e4_touch(v, d) ? -5 : 0;
    if (!rc && (grow || nsplit)) serial_printf("[ext4ro] htree ino %u: %s\n", d->ino, grow ? "root full - index now 2 levels" : "index node split");
out:
    if (rb) kfree(rb);
    if (nb) kfree(nb);
    if (lb) kfree(lb);
    if (xb) kfree(xb);
    if (yb) kfree(yb);
    if (ob) kfree(ob);
    if (m) kfree(m);
    return rc;
}
/* Linear insert of (name -> ino) into directory @d; grows it by a block when full. */
static int e4_dir_add(e4_vol_t *v, e4_inode_t *d, const char *nm, int nl, uint32_t ino, uint8_t ft) {
    if (!(v->incompat & I_FILETYPE)) ft = 0;
    if (d->flags & FL_INLINE) return -95;
    if (d->flags & FL_INDEX) return e4_dx_add(v, d, nm, nl, ino, ft);
    uint32_t need = (8u + nl + 3) & ~3u;
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -12;
    uint32_t nblk = (uint32_t)(d->size / v->bs);
    for (uint32_t lb = 0; lb < nblk; lb++) {
        uint64_t pb = e4_bmap(v, d, lb);
        if (pb == E4_BMAP_ERR) { kfree(b); return -5; }
        if (!pb) continue;
        if (e4_read(v, pb, b) != 0) { kfree(b); return -5; }
        if (v->csum && !e4_dirblk_ok(v, d, lb, pb, b)) { kfree(b); return -5; }
        uint32_t limit = v->csum ? v->bs - 12 : v->bs;
        for (uint32_t o = 0; o < limit; ) {
            uint32_t ei = *(uint32_t *)(b + o); uint16_t rl = *(uint16_t *)(b + o + 4); uint8_t enl = b[o + 6];
            if (rl < 8 || o + rl > v->bs) { kfree(b); return -5; }
            uint32_t act = ei ? ((8u + enl + 3) & ~3u) : 0;
            if (o + rl <= limit && rl >= act + need) {
                uint8_t *e = b + o;
                if (ei) { *(uint16_t *)(e + 4) = (uint16_t)act; e += act; *(uint16_t *)(e + 4) = (uint16_t)(rl - act); }
                *(uint32_t *)e = ino; e[6] = (uint8_t)nl; e[7] = ft; memcpy(e + 8, nm, nl);
                e4_dir_csum(v, d, b);
                int r = e4_wblk(v, pb, b) ? -5 : 0;
                kfree(b);
                return r ? r : (e4_touch(v, d) ? -5 : 0);
            }
            o += rl;
        }
    }
    /* no room: append a directory block */
    uint64_t goal; uint32_t end = e4_map_end(v, d, &goal);
    if (end == E4_NO_APPEND || end != nblk) { kfree(b); return -95; }
    uint64_t pb = e4_alloc_block(v, goal);
    if (!pb) { kfree(b); return -28; }
    memset(b, 0, v->bs);
    uint32_t limit = v->csum ? v->bs - 12 : v->bs;
    *(uint32_t *)b = ino; *(uint16_t *)(b + 4) = (uint16_t)limit; b[6] = (uint8_t)nl; b[7] = ft; memcpy(b + 8, nm, nl);
    if (v->csum) { uint8_t *t = b + v->bs - 12; *(uint16_t *)(t + 4) = 12; t[7] = 0xDE; }
    e4_dir_csum(v, d, b);
    if (e4_wblk(v, pb, b) != 0) { e4_free_block(v, pb); kfree(b); return -5; }
    kfree(b);
    int r = e4_map_append(v, d, nblk, pb);
    if (r) { e4_free_block(v, pb); return r; }
    e4_iblocks_add(v, d, 1);
    d->size += v->bs;
    return e4_touch(v, d) ? -5 : 0;
}
static int e4_create_in(e4_vol_t *v, const char *rel, uint16_t mode) {
    if (!v->rw) return -30;
    char dir[512]; const char *nm;
    int nl = e4_split(rel, dir, sizeof(dir), &nm);
    if (nl < 0) return nl;
    e4_inode_t *d = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!d) return -12;
    int rc = 0;
    uint32_t ino = 0;
    uint8_t *raw = NULL;
    if (!e4_namei(v, dir, d, 0, true) || (d->mode & S_IFMT_) != S_IFDIR_) { rc = -2; goto out; }
    if (d->flags & FL_INLINE) {
        serial_printf("[ext4ro] create in inline directory refused (ino %u)\n", d->ino);
        rc = -95; goto out;
    }
    if (e4_dir_lookup(v, d, nm, nl)) { rc = -17; goto out; }
    if (e4_mark_dirty(v) != 0) { rc = -5; goto out; }
    ino = e4_ialloc(v, (d->ino - 1) / v->ipg);
    if (!ino) { rc = -28; goto out; }
    raw = (uint8_t *)kalloc(v->isz);
    if (!raw) { rc = -12; goto out; }
    memset(raw, 0, v->isz);
    uint32_t now = (uint32_t)rtc_unix_epoch();
    *(uint16_t *)(raw + 0x00) = mode;
    *(uint32_t *)(raw + 0x08) = now; *(uint32_t *)(raw + 0x0C) = now; *(uint32_t *)(raw + 0x10) = now;
    *(uint16_t *)(raw + 0x1A) = 1;
    uint32_t gen; __asm__ volatile("rdtsc" : "=a"(gen) : : "edx");
    *(uint32_t *)(raw + 0x64) = gen ^ ino;
    if (v->incompat & I_EXTENTS) {
        *(uint32_t *)(raw + 0x20) = FL_EXTENTS;
        *(uint16_t *)(raw + 0x28) = 0xF30A; *(uint16_t *)(raw + 0x2C) = 4;
    }
    if (v->isz > 128) {
        uint16_t ex = v->want_extra ? v->want_extra : 32;
        if (128u + ex > v->isz) ex = (uint16_t)(v->isz - 128);
        *(uint16_t *)(raw + 0x80) = ex;
        if (ex >= 0x18) *(uint32_t *)(raw + 0x90) = now;           /* crtime */
    }
    if (e4_iwrite(v, ino, raw) != 0) { e4_ifree(v, ino); rc = -5; goto out; }
    rc = e4_dir_add(v, d, nm, nl, ino, 1);
    if (rc) { *(uint16_t *)(raw + 0x1A) = 0; *(uint32_t *)(raw + 0x14) = now; e4_iwrite(v, ino, raw); e4_ifree(v, ino); }
out:
    if (raw) kfree(raw);
    kfree(d);
    return rc;
}
static void e4_gd_dirs(e4_vol_t *v, uint32_t ino, int delta) {
    uint32_t g = (ino - 1) / v->ipg;
    uint8_t *d = v->gdt + (uint64_t)g * v->dsz;
    uint32_t n = *(uint16_t *)(d + 0x10) | (v->dsz >= 64 ? (uint32_t)*(uint16_t *)(d + 0x30) << 16 : 0);
    n += (uint32_t)delta;
    *(uint16_t *)(d + 0x10) = (uint16_t)n;
    if (v->dsz >= 64) *(uint16_t *)(d + 0x30) = (uint16_t)(n >> 16);
    if (v->csum) *(uint16_t *)(d + 0x1E) = e4_gd_csum(v, g);
    e4_gdt_write(v, g);
}
static uint32_t e4_dir_lookup(e4_vol_t *v, const e4_inode_t *d, const char *nm, int nl) {
    e4_find_t f = { nm, nl, 0 };
    if ((d->flags & FL_INDEX) && !(nl <= 2 && nm[0] == '.')) { int dr = e4_dx_find(v, d, nm, nl, &f.ino); if (dr >= 0) return dr ? f.ino : 0; }
    return e4_dir_iter(v, d, e4_find_cb, &f) == 1 ? f.ino : 0;
}
/* Remove entry @nm from directory @d (linear scan; htree leaves too). */
static int e4_dir_del2(e4_vol_t *v, e4_inode_t *d, const char *nm, int nl, uint32_t newino, uint8_t ft) {
    if (d->flags & FL_INLINE) return -95;
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -12;
    int rc = -2;
    uint32_t nblk = (uint32_t)((d->size + v->bs - 1) / v->bs);
    for (uint32_t lb = 0; lb < nblk && rc == -2; lb++) {
        uint64_t pb = e4_bmap(v, d, lb);
        if (pb == E4_BMAP_ERR) { rc = -5; break; }
        if (!pb) continue;
        if (e4_read(v, pb, b) != 0) { rc = -5; break; }
        if (v->csum && !e4_dirblk_ok(v, d, lb, pb, b)) { rc = -5; break; }
        int prev = -1;
        for (uint32_t o = 0; o + 8 <= v->bs; ) {
            uint32_t ei = *(uint32_t *)(b + o); uint16_t rl = *(uint16_t *)(b + o + 4); uint8_t enl = b[o + 6];
            if (rl < 8 || o + rl > v->bs) break;
            if (ei && enl == nl && !memcmp(b + o + 8, nm, nl)) {
                if (newino) { *(uint32_t *)(b + o) = newino; if (v->incompat & I_FILETYPE) b[o + 7] = ft; }
                else if (prev < 0) *(uint32_t *)(b + o) = 0;
                else *(uint16_t *)(b + prev + 4) += rl;
                e4_dir_csum(v, d, b);
                rc = e4_wblk(v, pb, b) ? -5 : 0;
                break;
            }
            prev = (int)o; o += rl;
        }
    }
    kfree(b);
    if (rc == 0) e4_touch(v, d);
    return rc;
}
static int e4_dir_del(e4_vol_t *v, e4_inode_t *d, const char *nm, int nl) { return e4_dir_del2(v, d, nm, nl, 0, 0); }
static int e4_empty_cb(const char *nm, int nl, uint32_t ino, uint8_t ft, void *arg) {
    if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.')) return 0;
    return 1;
}
/* Drop one link of inode @t (already removed from its directory). */
static int e4_release(e4_vol_t *v, e4_inode_t *t, bool isdir) {
    uint32_t now = (uint32_t)rtc_unix_epoch();
    if (!isdir && t->links > 1) {
        *(uint16_t *)(t->raw + 0x1A) = (uint16_t)(t->links - 1);
        *(uint32_t *)(t->raw + 0x0C) = now;
        return e4_iwrite(v, t->ino, t->raw) ? -5 : 0;
    }
    bool has_blocks = (t->mode & S_IFMT_) == S_IFREG_ || (t->mode & S_IFMT_) == S_IFDIR_ ||
                      (t->flags & FL_EXTENTS) || *(uint32_t *)(t->raw + 0x1C);
    if (has_blocks && !(t->flags & FL_INLINE)) {
        int r = e4_trunc_blocks(v, t, 0);
        if (r) return r;
    }
    memcpy(t->raw + 0x28, t->iblock, 60);
    *(uint16_t *)(t->raw + 0x1A) = 0;
    *(uint32_t *)(t->raw + 0x14) = now;                            /* dtime */
    *(uint32_t *)(t->raw + 0x0C) = now;
    *(uint32_t *)(t->raw + 0x04) = 0; *(uint32_t *)(t->raw + 0x6C) = 0;
    if (e4_iwrite(v, t->ino, t->raw) != 0) return -5;
    e4_ifree(v, t->ino);
    if (isdir) e4_gd_dirs(v, t->ino, -1);
    return 0;
}
/* ---- orphan list (s_last_orphan, next in i_dtime): unlinked inodes still open ---- */
static uint32_t e4_sb_get32(e4_vol_t *v, uint32_t off) {
    uint8_t *sb = (uint8_t *)kalloc(1024); uint32_t x = 0;
    if (sb && e4_sread(v, v->part + 2, 2, sb) == 0) x = *(uint32_t *)(sb + off);
    if (sb) kfree(sb);
    return x;
}
static int e4_sb_set32(e4_vol_t *v, uint32_t off, uint32_t x) {
    uint8_t *sb = (uint8_t *)kalloc(1024); int rc = -1;
    if (sb && e4_sread(v, v->part + 2, 2, sb) == 0 && *(uint16_t *)(sb + 56) == 0xEF53) {
        *(uint32_t *)(sb + off) = x;
        if (v->csum) *(uint32_t *)(sb + 0x3FC) = e4_crc32c(~0u, sb, 0x3FC);
        rc = (v->jw && v->jw->depth) ? e4_swrite(v, v->part + 2, 2, sb) : storage_write_sectors(v->dev, v->part + 2, 2, sb);
    }
    if (sb) kfree(sb);
    return rc ? -5 : 0;
}
static int e4_orphan_add(e4_vol_t *v, e4_inode_t *t) {
    uint32_t now = (uint32_t)rtc_unix_epoch();
    *(uint16_t *)(t->raw + 0x1A) = 0; t->links = 0;
    *(uint32_t *)(t->raw + 0x14) = e4_sb_get32(v, 0xE8);           /* dtime = next orphan */
    *(uint32_t *)(t->raw + 0x0C) = now;
    if (e4_iwrite(v, t->ino, t->raw) != 0) return -5;
    if (v->jw) v->jw->norphan++;
    return e4_sb_set32(v, 0xE8, t->ino);
}
static int e4_orphan_del(e4_vol_t *v, uint32_t ino, uint32_t next) {
    uint32_t cur = e4_sb_get32(v, 0xE8);
    if (cur == ino) return e4_sb_set32(v, 0xE8, next);
    e4_inode_t *c = (e4_inode_t *)kalloc(sizeof(e4_inode_t)); int rc = -5;
    for (uint32_t guard = 0; c && cur && guard < 65536; guard++) {
        if (e4_iget(v, cur, c) != 0) break;
        uint32_t n = *(uint32_t *)(c->raw + 0x14);
        if (n == ino) { *(uint32_t *)(c->raw + 0x14) = next; rc = e4_iwrite(v, cur, c->raw) ? -5 : 0; break; }
        cur = n;
    }
    if (c) kfree(c);
    return rc;
}
/* last close of an unlinked inode (or orphan found at mount): free it */
static int e4_release(e4_vol_t *v, e4_inode_t *t, bool isdir);
static int e4_orphan_release(e4_vol_t *v, e4_inode_t *t) {
    if (t->links) return 0;
    if (e4_orphan_del(v, t->ino, *(uint32_t *)(t->raw + 0x14)) != 0) return -5;
    return e4_release(v, t, false);
}
static void e4_orphans_process(e4_vol_t *v) {
    uint32_t ino = e4_sb_get32(v, 0xE8), n = 0;
    if (!ino) return;
    e4_inode_t *t = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!t) return;
    e4_tx_begin(v);
    for (uint32_t guard = 0; ino && guard < 65536; guard++) {
        if (ino < v->first_ino || ino > v->ipg * v->groups || e4_iget(v, ino, t) != 0) { e4_sb_set32(v, 0xE8, 0); break; }
        uint32_t next = *(uint32_t *)(t->raw + 0x14);
        if (e4_sb_set32(v, 0xE8, next) != 0) break;
        if (t->links == 0) e4_release(v, t, false);
        else {                                                   /* truncate orphan: drop blocks past i_size */
            if ((t->mode & S_IFMT_) == S_IFREG_ && !(t->flags & FL_INLINE)) e4_trunc_blocks(v, t, (uint32_t)((t->size + v->bs - 1) / v->bs));
            *(uint32_t *)(t->raw + 0x14) = 0; e4_touch(v, t);
        }
        n++; ino = next;
    }
    int r = e4_tx_end(v);
    kfree(t);
    printf("ext4 '%s': %u orphan inode(s) processed%s\n", v->label, n, r ? " (commit failed)" : "");
    serial_printf("[ext4ro] '%s': %u orphan inode(s) processed\n", v->label, n);
}
static void e4_link_adj(e4_vol_t *v, e4_inode_t *d, int delta) {
    if (delta > 0) {
        if (d->links >= 64999 || d->links == 1) d->links = (v->ro_compat & 0x0020) ? 1 : d->links + 1;   /* dir_nlink */
        else d->links++;
    } else if (d->links > 2) d->links--;
}
/* unlink (want_dir false) / rmdir (want_dir true) */
static int e4_unlink_in(e4_vol_t *v, const char *rel, bool busy, bool want_dir) {
    if (!v->rw) return -30;
    char dir[512]; const char *nm;
    int nl = e4_split(rel, dir, sizeof(dir), &nm);
    if (nl < 0) return nl;
    if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.')) return -22;
    e4_inode_t *d = (e4_inode_t *)kalloc(sizeof(e4_inode_t)), *t = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    int rc = -2;
    if (!d || !t) { rc = -12; goto out; }
    if (!e4_namei(v, dir, d, 0, true) || (d->mode & S_IFMT_) != S_IFDIR_) goto out;
    uint32_t ino = e4_dir_lookup(v, d, nm, nl);
    if (!ino) goto out;
    if (e4_iget(v, ino, t) != 0) { rc = -5; goto out; }
    bool isdir = (t->mode & S_IFMT_) == S_IFDIR_;
    if (want_dir && !isdir) { rc = -20; goto out; }
    if (!want_dir && isdir) { rc = -21; goto out; }
    if (*(uint32_t *)(t->raw + 0x68) || *(uint16_t *)(t->raw + 0x76)) { rc = -95; goto out; }   /* xattr block */
    if (isdir) {
        if (t->flags & FL_INLINE) { rc = -95; goto out; }
        int e = e4_dir_iter(v, t, e4_empty_cb, NULL);
        if (e < 0) { rc = -5; goto out; }
        if (e == 1) { rc = -39; goto out; }                        /* ENOTEMPTY */
    }
    if (busy && (isdir || !v->jw)) { rc = -16; goto out; }   /* journaled: an open file becomes an orphan */
    if (e4_mark_dirty(v) != 0) { rc = -5; goto out; }
    if ((rc = e4_dir_del(v, d, nm, nl)) != 0) goto out;
    if (isdir) { e4_iget(v, d->ino, d); e4_link_adj(v, d, -1); e4_touch(v, d); }
    if (busy && t->links <= 1) rc = e4_orphan_add(v, t);
    else rc = e4_release(v, t, isdir);
out:
    if (t) kfree(t);
    if (d) kfree(d);
    return rc;
}
static int e4_mkdir_in(e4_vol_t *v, const char *rel) {
    if (!v->rw) return -30;
    char dir[512]; const char *nm;
    int nl = e4_split(rel, dir, sizeof(dir), &nm);
    if (nl < 0) return nl;
    e4_inode_t *d = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    uint8_t *raw = (uint8_t *)kalloc(v->isz), *b = (uint8_t *)kalloc(v->bs);
    int rc = 0; uint32_t ino = 0; uint64_t pb = 0;
    if (!d || !raw || !b) { rc = -12; goto out; }
    if (!e4_namei(v, dir, d, 0, true) || (d->mode & S_IFMT_) != S_IFDIR_) { rc = -2; goto out; }
    if (d->flags & FL_INLINE) { rc = -95; goto out; }
    if (e4_dir_lookup(v, d, nm, nl)) { rc = -17; goto out; }
    if (!(v->ro_compat & 0x0020) && d->links >= 65000) { rc = -31; goto out; }
    if (e4_mark_dirty(v) != 0) { rc = -5; goto out; }
    ino = e4_ialloc(v, (d->ino - 1) / v->ipg);
    if (!ino) { rc = -28; goto out; }
    uint32_t g = (ino - 1) / v->ipg;
    pb = e4_alloc_block(v, v->first_data + (uint64_t)g * v->bpg);
    if (!pb) { e4_ifree(v, ino); rc = -28; goto out; }
    /* the new directory's block: ".", "..", (tail) */
    memset(b, 0, v->bs);
    uint8_t ft = (v->incompat & I_FILETYPE) ? 2 : 0;
    uint32_t limit = v->csum ? v->bs - 12 : v->bs;
    *(uint32_t *)b = ino; *(uint16_t *)(b + 4) = 12; b[6] = 1; b[7] = ft; b[8] = '.';
    *(uint32_t *)(b + 12) = d->ino; *(uint16_t *)(b + 16) = (uint16_t)(limit - 12); b[18] = 2; b[19] = ft; b[20] = '.'; b[21] = '.';
    if (v->csum) { uint8_t *t = b + v->bs - 12; *(uint16_t *)(t + 4) = 12; t[7] = 0xDE; }
    uint32_t now = (uint32_t)rtc_unix_epoch();
    memset(raw, 0, v->isz);
    *(uint16_t *)(raw + 0x00) = 0x41ED;
    *(uint32_t *)(raw + 0x04) = v->bs;
    *(uint32_t *)(raw + 0x08) = now; *(uint32_t *)(raw + 0x0C) = now; *(uint32_t *)(raw + 0x10) = now;
    *(uint16_t *)(raw + 0x1A) = 2;
    *(uint32_t *)(raw + 0x1C) = v->bs / 512;
    uint32_t gen; __asm__ volatile("rdtsc" : "=a"(gen) : : "edx");
    *(uint32_t *)(raw + 0x64) = gen ^ ino;
    if (v->incompat & I_EXTENTS) {
        *(uint32_t *)(raw + 0x20) = FL_EXTENTS;
        uint8_t *h = raw + 0x28;
        *(uint16_t *)h = 0xF30A; *(uint16_t *)(h + 2) = 1; *(uint16_t *)(h + 4) = 4;
        *(uint32_t *)(h + 12) = 0; *(uint16_t *)(h + 16) = 1; *(uint16_t *)(h + 18) = (uint16_t)(pb >> 32); *(uint32_t *)(h + 20) = (uint32_t)pb;
    } else *(uint32_t *)(raw + 0x28) = (uint32_t)pb;
    if (v->isz > 128) {
        uint16_t ex = v->want_extra ? v->want_extra : 32;
        if (128u + ex > v->isz) ex = (uint16_t)(v->isz - 128);
        *(uint16_t *)(raw + 0x80) = ex;
        if (ex >= 0x18) *(uint32_t *)(raw + 0x90) = now;
    }
    /* dirent checksum needs the new inode's seed */
    if (v->csum) {
        uint32_t le = ino, ge = *(uint32_t *)(raw + 0x64);
        uint32_t seed = e4_crc32c(e4_crc32c(v->seed, &le, 4), &ge, 4);
        *(uint32_t *)(b + v->bs - 4) = e4_crc32c(seed, b, v->bs - 12);
    }
    if (e4_wblk(v, pb, b) != 0 || e4_iwrite(v, ino, raw) != 0) { e4_free_block(v, pb); e4_ifree(v, ino); rc = -5; goto out; }
    e4_gd_dirs(v, ino, 1);
    rc = e4_dir_add(v, d, nm, nl, ino, 2);
    if (rc) {
        *(uint16_t *)(raw + 0x1A) = 0; *(uint32_t *)(raw + 0x14) = now; e4_iwrite(v, ino, raw);
        e4_free_block(v, pb); e4_ifree(v, ino); e4_gd_dirs(v, ino, -1);
        goto out;
    }
    e4_iget(v, d->ino, d);
    e4_link_adj(v, d, 1);
    rc = e4_touch(v, d) ? -5 : 0;
out:
    if (b) kfree(b);
    if (raw) kfree(raw);
    if (d) kfree(d);
    return rc;
}
/* point ".." of directory @c at @parent */
static int e4_set_dotdot(e4_vol_t *v, e4_inode_t *c, uint32_t parent) {
    uint64_t pb = e4_bmap(v, c, 0);
    if (!pb || pb == E4_BMAP_ERR) return -5;
    uint8_t *b = (uint8_t *)kalloc(v->bs);
    if (!b) return -12;
    int rc = -5;
    if (e4_read(v, pb, b) == 0 && (!v->csum || e4_dirblk_ok(v, c, 0, pb, b)) &&
        b[6] == 1 && b[8] == '.' && *(uint16_t *)(b + 4) == 12 && b[18] == 2 && b[20] == '.' && b[21] == '.') {
        *(uint32_t *)(b + 12) = parent;
        if (v->csum) {
            if (e4_has_tail(v, b)) e4_dir_csum(v, c, b);
            else if (c->flags & FL_INDEX) {                        /* dx_root: recompute dx_tail */
                uint32_t co = 24 + b[24 + 5], limit = *(uint16_t *)(b + co), count = *(uint16_t *)(b + co + 2), to = co + limit * 8;
                uint32_t cs = e4_crc32c(c->cseed, b, co + count * 8);
                cs = e4_crc32c(cs, b + to, 4); cs = e4_crc32c(cs, e4_zero4, 4);
                *(uint32_t *)(b + to + 4) = cs;
            } else goto done;
        }
        rc = e4_wblk(v, pb, b) ? -5 : 0;
    }
done:
    kfree(b);
    return rc;
}
static int e4_rename_in(e4_vol_t *v, const char *orel, const char *nrel, bool tbusy) {
    if (!v->rw) return -30;
    char od[512], nd[512]; const char *on, *nn;
    int ol = e4_split(orel, od, sizeof(od), &on), nlen = e4_split(nrel, nd, sizeof(nd), &nn);
    if (ol < 0) return ol;
    if (nlen < 0) return nlen;
    if ((ol <= 2 && on[0] == '.' && (ol == 1 || on[1] == '.')) || (nlen <= 2 && nn[0] == '.' && (nlen == 1 || nn[1] == '.'))) return -22;
    e4_inode_t *P = (e4_inode_t *)kalloc(sizeof(e4_inode_t)), *N = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    e4_inode_t *S = (e4_inode_t *)kalloc(sizeof(e4_inode_t)), *T = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    int rc = 0;
    if (!P || !N || !S || !T) { rc = -12; goto out; }
    if (!e4_namei(v, od, P, 0, true) || (P->mode & S_IFMT_) != S_IFDIR_) { rc = -2; goto out; }
    if (!e4_namei(v, nd, N, 0, true) || (N->mode & S_IFMT_) != S_IFDIR_) { rc = -2; goto out; }
    uint32_t sino = e4_dir_lookup(v, P, on, ol);
    if (!sino) { rc = -2; goto out; }
    if (e4_iget(v, sino, S) != 0) { rc = -5; goto out; }
    bool sdir = (S->mode & S_IFMT_) == S_IFDIR_;
    uint32_t tino = e4_dir_lookup(v, N, nn, nlen);
    if (tino == sino) goto out;                                    /* same inode: nothing to do */
    if (sdir) {                                                    /* not into its own subtree */
        uint32_t cur = N->ino;
        e4_inode_t *w = T;
        for (int k = 0; k < 256; k++) {
            if (cur == sino) { rc = -22; goto out; }
            if (cur == 2) break;
            if (e4_iget(v, cur, w) != 0) { rc = -5; goto out; }
            cur = e4_dir_lookup(v, w, "..", 2);
            if (!cur) { rc = -5; goto out; }
        }
    }
    if (N->flags & FL_INLINE) { rc = -95; goto out; }
    if (tino) {
        if (e4_iget(v, tino, T) != 0) { rc = -5; goto out; }
        bool tdir = (T->mode & S_IFMT_) == S_IFDIR_;
        if (sdir && !tdir) { rc = -20; goto out; }
        if (!sdir && tdir) { rc = -21; goto out; }
        if (tdir) { int e = e4_dir_iter(v, T, e4_empty_cb, NULL); if (e) { rc = e < 0 ? -5 : -39; goto out; } }
        if (!tdir && tbusy) { rc = -16; goto out; }
        if (*(uint32_t *)(T->raw + 0x68) || *(uint16_t *)(T->raw + 0x76)) { rc = -95; goto out; }
    }
    if (sdir && !(v->ro_compat & 0x0020) && N->ino != P->ino && N->links >= 65000 && !tino) { rc = -31; goto out; }
    if (e4_mark_dirty(v) != 0) { rc = -5; goto out; }
    uint8_t ft = sdir ? 2 : (S->mode & S_IFMT_) == S_IFLNK_ ? 7 : 1;
    if (tino) {
        /* replace: repoint the existing entry at the source inode */
        bool tdir = (T->mode & S_IFMT_) == S_IFDIR_;
        if ((rc = e4_dir_del2(v, N, nn, nlen, sino, ft)) != 0) goto out;   /* same name/hash: htree-safe */
        e4_iget(v, N->ino, N);
        if (tdir) { e4_link_adj(v, N, -1); e4_touch(v, N); }
        if ((rc = e4_release(v, T, tdir)) != 0) goto out;
    } else {
        if ((rc = e4_dir_add(v, N, nn, nlen, sino, ft)) != 0) goto out;
    }
    e4_iget(v, P->ino, P);
    if ((rc = e4_dir_del(v, P, on, ol)) != 0) goto out;
    if (sdir && P->ino != N->ino) {
        e4_iget(v, sino, S);
        if ((rc = e4_set_dotdot(v, S, N->ino)) != 0) goto out;
        e4_iget(v, P->ino, P); e4_link_adj(v, P, -1); e4_touch(v, P);
        e4_iget(v, N->ino, N); e4_link_adj(v, N, 1); e4_touch(v, N);
    }
    e4_iget(v, sino, S);
    *(uint32_t *)(S->raw + 0x0C) = (uint32_t)rtc_unix_epoch();     /* ctime */
    e4_iwrite(v, sino, S->raw);
out:
    if (P) kfree(P);
    if (N) kfree(N);
    if (S) kfree(S);
    if (T) kfree(T);
    return rc;
}

/* ---- VFS adapter ---- */
#define E4_MAX_H 32
typedef struct { bool in_use; e4_vol_t *v; e4_inode_t in; uint64_t pos; } e4_h_t;
static e4_h_t e4_h[E4_MAX_H];

/* propagate an inode change to the other open handles on it */
static void e4_hsync(e4_vol_t *v, const e4_inode_t *in, const e4_h_t *skip) {
    for (int i = 0; i < E4_MAX_H; i++)
        if (e4_h[i].in_use && &e4_h[i] != skip && e4_h[i].v == v && e4_h[i].in.ino == in->ino)
            memcpy(&e4_h[i].in, in, sizeof(*in));
}
static void *e4_open(vfs_filesystem_t *fs, const char *rel, uint32_t flags) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    if ((flags & (VFS_O_WRITE | VFS_O_APPEND | VFS_O_TRUNC)) && !v->rw) return NULL;
    int s = -1;
    for (int i = 0; i < E4_MAX_H; i++) if (!e4_h[i].in_use) { s = i; break; }
    if (s < 0) return NULL;
    e4_h_t *h = &e4_h[s];
    if (!e4_namei(v, rel, &h->in, 0, true) || (h->in.mode & S_IFMT_) == S_IFDIR_) return NULL;
    if ((flags & VFS_O_TRUNC) && h->in.size && e4_trunc(v, &h->in, 0) != 0) return NULL;
    h->in_use = true; h->v = v; h->pos = 0;
    e4_hsync(v, &h->in, h);
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
static int e4_write(vfs_filesystem_t *fs, void *hd, const void *b, uint32_t n) {
    e4_h_t *h = e4_hh(hd); if (!h) return -9;
    int r = e4_pwrite(h->v, &h->in, h->pos, (const uint8_t *)b, n);
    if (r > 0) h->pos += (uint32_t)r;
    e4_hsync(h->v, &h->in, h);
    return r;
}
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
/* Real inode metadata for stat(): follows symlinks like stat(2). */
static int e4_stat(vfs_filesystem_t *fs, const char *rel, vfs_stat_t *st) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    e4_inode_t *in = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!in) return -12;
    uint32_t ino = e4_namei(v, rel ? rel : "", in, 0, true);
    if (!ino) { kfree(in); return -2; /* ENOENT */ }
    const uint8_t *r = in->raw;
    memset(st, 0, sizeof(*st));
    st->mode = in->mode; st->uid = in->uid; st->gid = in->gid;
    st->nlink = in->links; st->size = in->size; st->ino = ino; st->blksize = v->bs;
    uint64_t blk = *(uint32_t *)(r + 0x1C);
    if (v->ro_compat & RO_HUGE_FILE) blk |= (uint64_t)*(uint16_t *)(r + 0x74) << 32;
    if (in->flags & FL_HUGE_FILE) blk *= v->bs / 512;
    st->blocks = blk;
    uint16_t extra = in->rawlen > 128 ? *(uint16_t *)(r + 0x80) : 0;
    int64_t at = (int32_t)*(uint32_t *)(r + 0x08), ct = (int32_t)*(uint32_t *)(r + 0x0C), mt = (int32_t)*(uint32_t *)(r + 0x10);
    /* extra timestamp words: low 2 bits extend the epoch past 2038 */
    if (128u + extra >= 0x88) ct += (int64_t)(*(uint32_t *)(r + 0x84) & 3) << 32;
    if (128u + extra >= 0x8C) mt += (int64_t)(*(uint32_t *)(r + 0x88) & 3) << 32;
    if (128u + extra >= 0x90) at += (int64_t)(*(uint32_t *)(r + 0x8C) & 3) << 32;
    st->atime = at; st->mtime = mt; st->ctime = ct;
    kfree(in);
    return 0;
}
static int e4_syncop(vfs_filesystem_t *fs) { return e4_sync_vol((e4_vol_t *)fs->fs_data) ? -5 : 0; }
static int e4_ro(vfs_filesystem_t *fs, const char *rel) { return -30; /* EROFS */ }
static int e4_createop(vfs_filesystem_t *fs, const char *rel) { return e4_create_in((e4_vol_t *)fs->fs_data, rel ? rel : "", 0x81A4); }
static int e4_unlinkop(vfs_filesystem_t *fs, const char *rel) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    e4_inode_t *t = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!t) return -12;
    bool busy = false;
    if (e4_namei(v, rel ? rel : "", t, 0, false))
        for (int i = 0; i < E4_MAX_H; i++) if (e4_h[i].in_use && e4_h[i].v == v && e4_h[i].in.ino == t->ino) busy = true;
    kfree(t);
    return e4_unlink_in(v, rel ? rel : "", busy, false);
}
static int e4_rmdirop(vfs_filesystem_t *fs, const char *rel) { return e4_unlink_in((e4_vol_t *)fs->fs_data, rel ? rel : "", false, true); }
static int e4_mkdirop(vfs_filesystem_t *fs, const char *rel) { return e4_mkdir_in((e4_vol_t *)fs->fs_data, rel ? rel : ""); }
static int e4_renameop(vfs_filesystem_t *fs, const char *a, const char *b) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    e4_inode_t *t = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!t) return -12;
    bool busy = false;
    if (e4_namei(v, b ? b : "", t, 0, false))
        for (int i = 0; i < E4_MAX_H; i++) if (e4_h[i].in_use && e4_h[i].v == v && e4_h[i].in.ino == t->ino) busy = true;
    kfree(t);
    return e4_rename_in(v, a ? a : "", b ? b : "", busy);
}
static int e4_truncop(vfs_filesystem_t *fs, const char *rel, uint64_t len) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    e4_inode_t *t = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!t) return -12;
    int r = e4_namei(v, rel ? rel : "", t, 0, true) ? e4_trunc(v, t, len) : -2;
    if (r == 0) e4_hsync(v, t, NULL);
    kfree(t);
    return r;
}
static int e4_ftruncop(vfs_filesystem_t *fs, void *hd, uint64_t len) {
    e4_h_t *h = e4_hh(hd); if (!h) return -9;
    int r = e4_trunc(h->v, &h->in, len);
    e4_hsync(h->v, &h->in, h);
    return r;
}
static int e4_ro2(vfs_filesystem_t *fs, const char *a, const char *b) { return -30; }
static int e4_ro3(vfs_filesystem_t *fs, const char *a, uint64_t l) { return -30; }

#define E4_TXOP(name, call, args, ...) \
    static int e4tx_##name(vfs_filesystem_t *fs, __VA_ARGS__) { e4_vol_t *v = (e4_vol_t *)fs->fs_data; e4_tx_begin(v); int r = call args; \
        if (e4_tx_end(v) != 0 && r >= 0) { r = -5; } return r; }
/* writes: one transaction per 1 MiB chunk (a crash can leave a prefix of a large write) */
#define E4_TX_CHUNK (1u << 20)
static int e4tx_write(vfs_filesystem_t *fs, void *hd, const void *b, uint32_t n) {
    e4_vol_t *v = (e4_vol_t *)fs->fs_data;
    if (!v->jw) return e4_write(fs, hd, b, n);
    uint32_t done = 0;
    do {
        uint32_t c = n - done > E4_TX_CHUNK ? E4_TX_CHUNK : n - done;
        e4_tx_begin(v);
        int r = e4_write(fs, hd, (const uint8_t *)b + done, c);
        if (e4_tx_end(v) != 0 && r >= 0) r = -5;
        if (r < 0) return done ? (int)done : r;
        done += (uint32_t)r;
        if ((uint32_t)r < c) break;
    } while (done < n);
    return (int)done;
}
E4_TXOP(create, e4_createop, (fs, rel), const char *rel)
E4_TXOP(mkdir, e4_mkdirop, (fs, rel), const char *rel)
E4_TXOP(unlink, e4_unlinkop, (fs, rel), const char *rel)
E4_TXOP(rename, e4_renameop, (fs, a, b), const char *a, const char *b)
E4_TXOP(rmdir, e4_rmdirop, (fs, rel), const char *rel)
/* close: the last close of an unlinked (orphan) inode frees it, in its own transaction */
static int e4tx_close(vfs_filesystem_t *fs, void *hd) {
    e4_h_t *h = e4_hh(hd); if (!h) return -1;
    e4_vol_t *v = h->v; uint32_t ino = h->in.ino;
    h->in_use = false;
    if (!v || !v->jw || !v->rw) return 0;
    for (int i = 0; i < E4_MAX_H; i++) if (e4_h[i].in_use && e4_h[i].v == v && e4_h[i].in.ino == ino) return 0;
    e4_inode_t *t = (e4_inode_t *)kalloc(sizeof(e4_inode_t));
    if (!t) return 0;
    if (e4_iget(v, ino, t) == 0 && t->links == 0 && t->mode) {
        e4_tx_begin(v);
        int r = e4_orphan_release(v, t);
        if (e4_tx_end(v) != 0 || r) serial_printf("[ext4ro] '%s': ino %u: orphan release failed (%d)\n", v->label, ino, r);
    }
    kfree(t);
    return 0;
}
E4_TXOP(trunc, e4_truncop, (fs, rel, len), const char *rel, uint64_t len)
E4_TXOP(ftrunc, e4_ftruncop, (fs, hd, len), void *hd, uint64_t len)
static vfs_fs_ops_t e4_ops = {
    .open = e4_open, .close = e4tx_close, .read = e4_readop, .write = e4tx_write,
    .seek = e4_seek, .get_size = e4_size, .readdir = e4_readdir,
    .create = e4tx_create, .mkdir = e4tx_mkdir, .unlink = e4tx_unlink, .exists = e4_exists,
    .rename = e4tx_rename, .rmdir = e4tx_rmdir, .truncate = e4tx_trunc, .ftruncate = e4tx_ftrunc,
    .stat = e4_stat, .sync = e4_syncop,
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
