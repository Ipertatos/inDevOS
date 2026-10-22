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
};

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

static int e4_read(e4_vol_t *v, uint64_t blk, void *buf) {
    return storage_read_sectors(v->dev, v->part + blk * v->spb, v->spb, buf);
}
static int e4_wblk(e4_vol_t *v, uint64_t blk, const void *buf) {
    if (!v->rw || blk < v->first_data + 1 || blk >= v->blocks) return -1;   /* never the superblock area */
    return storage_write_sectors(v->dev, v->part + blk * v->spb, v->spb, buf);
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
    if (inc & I_RECOVER) printf("ext4: journal needs recovery - reading as-is (read-only)\n");
    serial_printf("[ext4ro] mounted dev %u LBA %u label '%s': %u-byte blocks, %u groups, incompat %#x ro_compat %#x%s\n",
                  dev, lba, v->label, v->bs, v->groups, inc, roc, v->csum ? ", metadata_csum verified" : "");
    return v;
}

static int e4_sync_vol(e4_vol_t *v);
void ext4ro_unmount(e4_vol_t *v) {
    if (!v) return;
    if (v->rw && e4_sync_vol(v) != 0) printf("ext4 '%s': could not mark the volume clean\n", v->label);
    for (int i = 0; i < E4_MAX_VOLS; i++) if (e4_vols[i] == v) e4_vols[i] = NULL;
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
    if (storage_read_sectors(v->dev, v->part + 2, 2, sb) != 0 || *(uint16_t *)(sb + 56) != 0xEF53) goto out;
    if (v->csum && e4_crc32c(~0u, sb, 0x3FC) != *(uint32_t *)(sb + 0x3FC)) { e4_csum_bad(v, "superblock", 0, *(uint32_t *)(sb + 0x3FC), 0); goto out; }
    uint16_t *st = (uint16_t *)(sb + 0x3A);
    if (clean) *st |= 1; else *st &= (uint16_t)~1u;
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
    if (storage_write_sectors(v->dev, v->part + 2, 2, sb) != 0) goto out;
    v->alloc_since_sync = 0; v->ialloc_since_sync = 0; v->sb_clean = clean; rc = 0;
out:
    kfree(sb);
    return rc;
}
static int e4_mark_dirty(e4_vol_t *v) { return v->sb_clean ? e4_sb_update(v, false) : 0; }
/* sync / unmount: free count + clean flag. */
static int e4_sync_vol(e4_vol_t *v) {
    if (!v || !v->rw) return 0;
    if (v->sb_clean && !v->alloc_since_sync && !v->ialloc_since_sync) return 0;
    return e4_sb_update(v, true);
}

int ext4ro_enable_rw(e4_vol_t *v) {
    if (!v) return -1;
    if (v->rw) return 0;
    uint8_t *sb = (uint8_t *)kalloc(1024);
    if (!sb) return -1;
    const char *why = NULL;
    uint32_t bad = 0;
    if (storage_read_sectors(v->dev, v->part + 2, 2, sb) != 0 || *(uint16_t *)(sb + 56) != 0xEF53) why = "superblock unreadable";
    else {
        uint32_t c = *(uint32_t *)(sb + 0x5C), inc = *(uint32_t *)(sb + 0x60), roc = *(uint32_t *)(sb + 0x64);
        uint16_t state = *(uint16_t *)(sb + 0x3A);
        uint32_t rev = *(uint32_t *)(sb + 0x4C);
        if (c & C_HAS_JOURNAL) why = "has a journal (has_journal)";
        else if (inc & I_RECOVER) why = "journal needs recovery";
        else if (!(state & 1) || (state & 2)) { why = "not cleanly unmounted or has errors - run e2fsck"; bad = state; }
        else if (*(uint32_t *)(sb + 0x194)) why = "error records present (s_error_count) - run e2fsck";
        else if (inc & I_INLINE) why = "inline_data";
        else if (inc & I_META_BG) why = "meta_bg";
        else if (roc & RO_BIGALLOC) why = "bigalloc";
        else if (inc & I_ENCRYPT) why = "encryption";
        else if (inc & I_CASEFOLD) why = "casefold";
        else if (inc & I_MMP) why = "mmp";
        else if (inc & ~I_RW_OK) { why = "unknown/unsupported INCOMPAT features"; bad = inc & ~I_RW_OK; }
        else if (roc & ~RO_RW_OK) { why = "unknown/unsupported RO_COMPAT features"; bad = roc & ~RO_RW_OK; }
        else if (c & ~C_RW_OK) { why = "unknown/unsupported COMPAT features"; bad = c & ~C_RW_OK; }
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
    if (e4_sb_update(v, false) != 0) { v->rw = false; printf("ext4 '%s': cannot write superblock - read-only\n", v->label); return -5; }
    serial_printf("[ext4ro] '%s' read-write enabled (state not-clean until sync/unmount)\n", v->label);
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
    if (storage_read_sectors(v->dev, lba, nsec, b) != 0) goto out;
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
    if (storage_write_sectors(v->dev, lba, nsec, b) != 0) goto out;
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
    if (storage_write_sectors(v->dev, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) != 0) return -1;
    uint64_t gblk = v->first_data + 1 + ((uint64_t)g * v->dsz) / v->bs;
    uint32_t goff = (uint32_t)(((uint64_t)g * v->dsz) / v->bs) * v->bs;
    if (storage_write_sectors(v->dev, v->part + gblk * v->spb, v->spb, v->gdt + goff) != 0) return -1;
    v->alloc_since_sync += delta;
    return 0;
}
/* Allocate one block, first free at/after @goal (wrapping).  0 = none. */
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
        if (!e4_gd_free(v, g) || (*(uint16_t *)(d + 0x12) & BG_BLOCK_UNINIT)) continue;
        if (storage_read_sectors(v->dev, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) != 0) break;
        if (v->csum) {
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
    uint32_t g = (uint32_t)((pb - v->first_data) / v->bpg), i = (uint32_t)((pb - v->first_data) % v->bpg);
    uint8_t *bm = (uint8_t *)kalloc(v->bs);
    if (!bm) return;
    if (storage_read_sectors(v->dev, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) == 0 &&
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
        const uint32_t *ib = (const uint32_t *)in->iblock;
        if (ib[12] || ib[13] || ib[14]) return E4_NO_APPEND;    /* indirect blocks: not yet */
        uint32_t e = 0;
        for (uint32_t i = 0; i < 12; i++) if (ib[i]) { e = i + 1; *goal = ib[i] + 1; }
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
static int e4_map_append(e4_vol_t *v, e4_inode_t *in, uint32_t lblk, uint64_t pb) {
    if (!(in->flags & FL_EXTENTS)) {
        if (lblk >= 12) return -95;
        ((uint32_t *)in->iblock)[lblk] = (uint32_t)pb;
        return 0;
    }
    uint8_t *root = in->iblock;
    if (*(uint16_t *)root != 0xF30A) return -5;
    uint16_t depth = *(uint16_t *)(root + 6);
    if (depth == 0) {
        int r = e4_leaf_add(root, lblk, pb);
        if (r == 0) return 0;
        /* split: root entries -> new leaf block, root becomes a 1-entry index */
        uint64_t nb = e4_alloc_block(v, pb + 1);
        if (!nb) return -28;
        uint8_t *blk = (uint8_t *)kalloc(v->bs);
        if (!blk) { e4_free_block(v, nb); return -12; }
        memset(blk, 0, v->bs);
        uint16_t n = *(uint16_t *)(root + 2);
        *(uint16_t *)blk = 0xF30A; *(uint16_t *)(blk + 2) = n;
        *(uint16_t *)(blk + 4) = (uint16_t)((v->bs - 12) / 12);
        *(uint16_t *)(blk + 6) = 0; *(uint32_t *)(blk + 8) = 0;
        memcpy(blk + 12, root + 12, 12u * n);
        e4_leaf_add(blk, lblk, pb);
        e4_leaf_csum(v, in, blk);
        if (e4_wblk(v, nb, blk) != 0) { kfree(blk); e4_free_block(v, nb); return -5; }
        kfree(blk);
        uint32_t first = n ? *(uint32_t *)(root + 12) : lblk;
        memset(root + 12, 0, 48);
        *(uint16_t *)(root + 2) = 1; *(uint16_t *)(root + 6) = 1;
        uint8_t *ix = root + 12;
        *(uint32_t *)ix = first; *(uint32_t *)(ix + 4) = (uint32_t)nb; *(uint16_t *)(ix + 8) = (uint16_t)(nb >> 32);
        e4_iblocks_add(v, in, 1);
        serial_printf("[ext4ro] ino %u: extent root full - split into leaf block %llu\n", in->ino, (unsigned long long)nb);
        return 0;
    }
    if (depth != 1) return -95;                                     /* deeper trees: not yet */
    uint16_t n = *(uint16_t *)(root + 2);
    if (!n) return -5;
    uint8_t *ix = root + 12 + 12 * (n - 1);
    uint64_t leaf = *(uint32_t *)(ix + 4) | ((uint64_t)*(uint16_t *)(ix + 8) << 32);
    uint8_t *blk = (uint8_t *)kalloc(v->bs);
    if (!blk) return -12;
    int rc = -5;
    if (e4_read(v, leaf, blk) == 0 && *(uint16_t *)blk == 0xF30A && *(uint16_t *)(blk + 6) == 0) {
        if (v->csum) {
            uint32_t to = 12 + 12u * *(uint16_t *)(blk + 4);
            if (to + 4 > v->bs || e4_crc32c(in->cseed, blk, to) != *(uint32_t *)(blk + to)) { e4_csum_bad(v, "extent block", leaf, 0, 0); goto out; }
        }
        if (e4_leaf_add(blk, lblk, pb) != 0) {                     /* leaf full: start the next leaf */
            if (n >= *(uint16_t *)(root + 4)) { serial_printf("[ext4ro] ino %u: extent index full (depth 2 not supported)\n", in->ino); rc = -95; goto out; }
            uint64_t nb = e4_alloc_block(v, pb + 1);
            if (!nb) { rc = -28; goto out; }
            memset(blk, 0, v->bs);
            *(uint16_t *)blk = 0xF30A; *(uint16_t *)(blk + 4) = (uint16_t)((v->bs - 12) / 12);
            e4_leaf_add(blk, lblk, pb);
            e4_leaf_csum(v, in, blk);
            if (e4_wblk(v, nb, blk) != 0) { e4_free_block(v, nb); rc = -5; goto out; }
            uint8_t *nx = root + 12 + 12 * n;
            memset(nx, 0, 12);
            *(uint32_t *)nx = lblk; *(uint32_t *)(nx + 4) = (uint32_t)nb; *(uint16_t *)(nx + 8) = (uint16_t)(nb >> 32);
            *(uint16_t *)(root + 2) = n + 1;
            e4_iblocks_add(v, in, 1);
            serial_printf("[ext4ro] ino %u: extent leaf %u full - new leaf block %llu\n", in->ino, (unsigned)n, (unsigned long long)nb);
            rc = 0; goto out;
        }
        e4_leaf_csum(v, in, blk);
        rc = e4_wblk(v, leaf, blk) == 0 ? 0 : -5;
    }
out:
    kfree(blk);
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
            if (e4_wblk(v, pb, b) != 0) { kfree(b); return -5; }
        }
    }
    while (done < len) {
        uint64_t p = off + done;
        uint32_t lb = (uint32_t)(p / v->bs), in_off = (uint32_t)(p % v->bs);
        uint32_t n = v->bs - in_off; if (n > len - done) n = len - done;
        uint64_t pb = e4_bmap(v, in, lb);
        if (pb == E4_BMAP_ERR) { err = -5; break; }
        if (!pb) {
            uint64_t goal;
            uint32_t end = e4_map_end(v, in, &goal);
            if (end == E4_NO_APPEND || lb < end) {
                serial_printf("[ext4ro] write into hole/unwritten block (ino %u lblk %u, map end %u): not supported yet\n", in->ino, lb, end);
                err = -95; break;
            }
            if (lb > end) goal += lb - end;
            pb = e4_alloc_block(v, goal);
            if (!pb) { err = -28; break; }
            memset(b, 0, v->bs);
            memcpy(b + in_off, src + done, n);
            if (e4_wblk(v, pb, b) != 0) { e4_free_block(v, pb); err = -5; break; }
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
            if (e4_wblk(v, pb, src + done) != 0) { err = -5; break; }
        } else {
            if (e4_read(v, pb, b) != 0) { err = -5; break; }
            memcpy(b + in_off, src + done, n);
            if (e4_wblk(v, pb, b) != 0) { err = -5; break; }
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
    return storage_write_sectors(v->dev, v->part + gblk * v->spb, v->spb, v->gdt + goff);
}
/* Free [start, start+cnt) in the block bitmaps (only bits that are set). */
static int e4_free_range(e4_vol_t *v, uint64_t start, uint32_t cnt) {
    uint8_t *bm = (uint8_t *)kalloc(v->bs);
    if (!bm) return -12;
    int rc = 0;
    while (cnt) {
        if (start < v->first_data + 1 || start >= v->blocks) { rc = -5; break; }
        uint32_t g = (uint32_t)((start - v->first_data) / v->bpg), i = (uint32_t)((start - v->first_data) % v->bpg);
        uint32_t n = v->bpg - i; if (n > cnt) n = cnt;
        if (storage_read_sectors(v->dev, v->part + e4_gd_bitmap(v, g) * v->spb, v->spb, bm) != 0) { rc = -5; break; }
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
static int e4_trunc_blocks(e4_vol_t *v, e4_inode_t *in, uint32_t keep) {
    uint32_t freed = 0;
    int rc = 0;
    if (in->flags & FL_EXTENTS) {
        uint8_t *root = in->iblock;
        if (*(uint16_t *)root != 0xF30A)                    /* empty file without an extent header */
            return (*(uint32_t *)(in->raw + 0x1C) == 0 && in->size == 0) ? 0 : -5;
        uint16_t depth = *(uint16_t *)(root + 6);
        if (depth == 0) rc = e4_leaf_trim(v, root, keep, &freed);
        else if (depth == 1) {
            uint16_t n = *(uint16_t *)(root + 2), w = 0;
            uint8_t *blk = (uint8_t *)kalloc(v->bs);
            if (!blk) return -12;
            for (uint16_t i = 0; i < n && !rc; i++) {
                uint8_t *ix = root + 12 + 12 * i;
                uint64_t leaf = *(uint32_t *)(ix + 4) | ((uint64_t)*(uint16_t *)(ix + 8) << 32);
                if (e4_read(v, leaf, blk) != 0 || *(uint16_t *)blk != 0xF30A) { rc = -5; break; }
                if (v->csum) {
                    uint32_t to = 12 + 12u * *(uint16_t *)(blk + 4);
                    if (to + 4 > v->bs || e4_crc32c(in->cseed, blk, to) != *(uint32_t *)(blk + to)) { e4_csum_bad(v, "extent block", leaf, 0, 0); rc = -5; break; }
                }
                if ((rc = e4_leaf_trim(v, blk, keep, &freed)) != 0) break;
                if (*(uint16_t *)(blk + 2) == 0 && !(i == 0 && keep > 0)) {
                    if (e4_free_range(v, leaf, 1)) { rc = -5; break; }
                    freed++;
                    continue;                                      /* drop the index entry */
                }
                e4_leaf_csum(v, in, blk);
                if (e4_wblk(v, leaf, blk) != 0) { rc = -5; break; }
                if (w != i) memmove(root + 12 + 12 * w, ix, 12);
                w++;
            }
            kfree(blk);
            if (!rc) {
                for (uint16_t i = w; i < n; i++) memset(root + 12 + 12 * i, 0, 12);
                *(uint16_t *)(root + 2) = w;
                if (w == 0) { *(uint16_t *)(root + 6) = 0; *(uint16_t *)(root + 4) = 4; }
            }
        } else return -95;
    } else {
        uint32_t *ib = (uint32_t *)in->iblock;
        if ((ib[12] || ib[13] || ib[14]) && keep > 12) return -95;   /* partial indirect trim: not yet */
        for (uint32_t i = keep; i < 12; i++) if (ib[i]) { if (e4_free_range(v, ib[i], 1)) return -5; ib[i] = 0; freed++; }
        for (int l = 0; l < 3 && !rc; l++) if (ib[12 + l]) { rc = e4_free_ind(v, ib[12 + l], l + 1, &freed); if (!rc) ib[12 + l] = 0; }
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
            e4_wblk(v, pb, b);
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
    if (storage_write_sectors(v->dev, v->part + e4_gd_ibitmap(v, g) * v->spb, v->spb, bm) != 0) return -1;
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
        } else if (storage_read_sectors(v->dev, v->part + e4_gd_ibitmap(v, g) * v->spb, v->spb, bm) != 0) break;
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
    if (storage_read_sectors(v->dev, v->part + e4_gd_ibitmap(v, g) * v->spb, v->spb, bm) == 0 && (bm[i >> 3] & (1u << (i & 7)))) {
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
    if (storage_read_sectors(v->dev, lba, nsec, b) == 0) {
        memcpy(b + off % 512, raw, v->isz);
        rc = storage_write_sectors(v->dev, lba, nsec, b) == 0 ? 0 : -1;
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
    if (busy) { rc = -16; goto out; }
    if (e4_mark_dirty(v) != 0) { rc = -5; goto out; }
    if ((rc = e4_dir_del(v, d, nm, nl)) != 0) goto out;
    if (isdir) { e4_iget(v, d->ino, d); e4_link_adj(v, d, -1); e4_touch(v, d); }
    rc = e4_release(v, t, isdir);
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

static vfs_fs_ops_t e4_ops = {
    .open = e4_open, .close = e4_close, .read = e4_readop, .write = e4_write,
    .seek = e4_seek, .get_size = e4_size, .readdir = e4_readdir,
    .create = e4_createop, .mkdir = e4_mkdirop, .unlink = e4_unlinkop, .exists = e4_exists,
    .rename = e4_renameop, .rmdir = e4_rmdirop, .truncate = e4_truncop, .ftruncate = e4_ftruncop,
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
