/* exFAT filesystem driver: mount, directory walk with long UTF-16 names,
 * case-insensitive lookup via the volume's upcase table, file reads over
 * contiguous (NoFatChain) and FAT-chained cluster runs. Read-only so far. */
#include "exfat.h"
#include "storage.h"
#include "partscan.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "rtc.h"

extern int printf(const char *fmt, ...);

#define EX_EOC      0xFFFFFFFFu
#define EX_BAD      0xFFFFFFF7u
#define EX_ATTR_DIR 0x10
#define EX_NOFATCHAIN 0x02

struct exfat_vol {
    uint8_t  dev;
    uint32_t part;
    uint32_t vol_len_lo;
    uint32_t fat_off, fat_len, heap_off, clus_count, root;
    uint32_t spc;           /* sectors per cluster */
    uint32_t cbytes;
    uint8_t  spc_shift;
    uint16_t *upcase;       /* 65536 entries */
    uint32_t bitmap_clus;
    uint64_t bitmap_len;
    char     label[48];
    uint32_t fat_cached;
    uint8_t  fat_sec[512];
    /* read-write state (only after exfat_enable_rw) */
    bool     rw;
    uint8_t *bm;            /* whole allocation bitmap, cluster-rounded */
    uint32_t bm_hint;
    bool     dirty_on;      /* VolumeDirty currently set on disk */
    char     mpath[64];     /* VFS mount path ("" = not registered) */
};

#define EX_MAX_VOLS 8
static exfat_vol_t *ex_vols[EX_MAX_VOLS];

/* A directory (or file) data run descriptor. */
typedef struct {
    uint32_t first;
    uint8_t  flags;
    uint64_t len;           /* 0 = follow chain to the end (root) */
} ex_run_t;

/* Entry + where its entry set lives (for rewriting it). */
typedef struct {
    uint16_t attr;
    uint8_t  flags;
    uint32_t first;
    uint64_t len, vdl;
    uint32_t ctime, mtime, atime;
    int      nlen;
    uint16_t name[256];
    ex_run_t parent;        /* directory holding the set */
    uint64_t set_off;       /* byte offset of the 0x85 entry in @parent */
    uint8_t  nsec;          /* secondary count */
    bool     is_root;
} ex_ent_t;

static uint32_t ex_lba(exfat_vol_t *v, uint32_t c) {
    return v->part + v->heap_off + (c - 2) * v->spc;
}

static uint32_t ex_fat_next(exfat_vol_t *v, uint32_t c) {
    uint32_t sec = v->part + v->fat_off + (c * 4u) / 512u;
    if (v->fat_cached != sec) {
        if (storage_read_sectors(v->dev, sec, 1, v->fat_sec) != 0) return EX_BAD;
        v->fat_cached = sec;
    }
    return *(uint32_t *)(v->fat_sec + (c * 4u) % 512u);
}

static bool ex_valid_clus(exfat_vol_t *v, uint32_t c) {
    return c >= 2 && c < v->clus_count + 2;
}

/* Cluster number of the idx-th cluster of a run, walking from a cached
 * (idx, cluster) position when possible. */
static uint32_t ex_run_clus(exfat_vol_t *v, const ex_run_t *r, uint32_t idx,
                            uint32_t *cache_idx, uint32_t *cache_clus) {
    if (r->flags & EX_NOFATCHAIN) return r->first + idx;
    uint32_t i = 0, c = r->first;
    if (cache_idx && *cache_clus && *cache_idx <= idx) { i = *cache_idx; c = *cache_clus; }
    while (i < idx) {
        c = ex_fat_next(v, c);
        if (!ex_valid_clus(v, c)) return 0;
        i++;
    }
    if (cache_idx) { *cache_idx = i; *cache_clus = c; }
    return c;
}

static int ex_set_volflags(exfat_vol_t *v, bool dirty);
#define ENOENT_EX 2
#define EBUSY_EX 16
#define ENOTDIR_EX 20
#define EISDIR_EX 21
#define EINVAL_EX 22
#define ENOSPC_EX 28
#define EROFS_EX 30
#define ENOTEMPTY_EX 39
static int ex_debug = 0;   /* run-1 diagnostics */
static uint16_t ex_up(exfat_vol_t *v, uint16_t c) { return v->upcase ? v->upcase[c] : (c >= 'a' && c <= 'z' ? c - 32 : c); }

/* ---- directory iteration ------------------------------------------------- */

/* Calls @cb for every complete file entry set in the directory @dir.
 * Returns the callback's non-zero value to stop early, 0 at end, <0 on error. */
typedef int (*ex_dir_cb)(exfat_vol_t *v, const ex_ent_t *e, void *arg);

static int ex_walk_dir(exfat_vol_t *v, const ex_run_t *dir, ex_dir_cb cb, void *arg) {
    static uint8_t sec[512];
    ex_ent_t *e = (ex_ent_t *)kalloc(sizeof(ex_ent_t));
    if (!e) return -1;
    int remaining = 0, got = 0, rc = 0;
    uint32_t ci = 0, cc = 0;
    uint64_t total = dir->len ? dir->len : 0xFFFFFFFFFFFFull;
    uint64_t off = 0;
    for (uint32_t idx = 0; off < total && idx < (1u << 20); idx++) {
        uint32_t c = ex_run_clus(v, dir, idx, &ci, &cc);
        if (!ex_valid_clus(v, c)) break;
        for (uint32_t s = 0; s < v->spc && off < total; s++, off += 512) {
            if (storage_read_sectors(v->dev, ex_lba(v, c) + s, 1, sec) != 0) { rc = -1; goto out; }
            for (uint32_t o = 0; o < 512; o += 32) {
                uint8_t t = sec[o];
                if (t == 0x00) goto out;                    /* end of directory */
                if (t == 0x85) {
                    memset(e, 0, sizeof(*e));
                    remaining = sec[o + 1];
                    e->nsec = sec[o + 1];
                    e->set_off = off + o;
                    e->parent = *dir;
                    e->attr = *(uint16_t *)(sec + o + 4);
                    e->ctime = *(uint32_t *)(sec + o + 8);
                    e->mtime = *(uint32_t *)(sec + o + 12);
                    e->atime = *(uint32_t *)(sec + o + 16);
                    got = 0;
                    continue;
                }
                if (!(t & 0x80) || remaining <= 0) { if (!(t & 0x80)) remaining = 0; continue; }
                remaining--;
                if (t == 0xC0) {
                    e->flags = sec[o + 1];
                    e->nlen = sec[o + 3];
                    e->vdl = *(uint64_t *)(sec + o + 8);
                    e->first = *(uint32_t *)(sec + o + 20);
                    e->len = *(uint64_t *)(sec + o + 24);
                } else if (t == 0xC1) {
                    for (int k = 0; k < 15 && got < e->nlen && got < 255; k++)
                        e->name[got++] = *(uint16_t *)(sec + o + 2 + 2 * k);
                }
                if (remaining == 0 && e->nlen > 0 && got >= e->nlen) {
                    rc = cb(v, e, arg);
                    if (rc) goto out;
                }
            }
        }
        if (!dir->len) {
            /* root: chain-terminated; check the next link exists */
            if (!(dir->flags & EX_NOFATCHAIN)) {
                uint32_t n = ex_fat_next(v, c);
                if (!ex_valid_clus(v, n)) break;
            }
        }
    }
out:
    kfree(e);
    return rc;
}

/* UTF-16 -> UTF-8 */
static void ex_name_utf8(const ex_ent_t *e, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < e->nlen && o < cap - 1; i++) {
        uint32_t cp = e->name[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < e->nlen &&
            e->name[i + 1] >= 0xDC00 && e->name[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (e->name[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) {
            if (o + 2 > cap - 1) break;
            out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 > cap - 1) break;
            out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 > cap - 1) break;
            out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = '\0';
}

/* UTF-8 component -> UTF-16; returns length or -1 */
static int ex_utf8_to16(const char *s, int n, uint16_t *out, int cap) {
    int o = 0;
    for (int i = 0; i < n;) {
        uint32_t cp; uint8_t b = (uint8_t)s[i];
        if (b < 0x80) { cp = b; i++; }
        else if ((b & 0xE0) == 0xC0 && i + 1 < n) { cp = ((b & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2; }
        else if ((b & 0xF0) == 0xE0 && i + 2 < n) { cp = ((b & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3; }
        else if ((b & 0xF8) == 0xF0 && i + 3 < n) {
            cp = ((b & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); i += 4;
        } else return -1;
        if (cp >= 0x10000) {
            if (o + 2 > cap) return -1;
            cp -= 0x10000; out[o++] = (uint16_t)(0xD800 + (cp >> 10)); out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            if (o + 1 > cap) return -1;
            out[o++] = (uint16_t)cp;
        }
    }
    return o;
}

typedef struct { const uint16_t *want; int wlen; ex_ent_t *found; } ex_find_t;

static int ex_find_cb(exfat_vol_t *v, const ex_ent_t *e, void *arg) {
    ex_find_t *f = (ex_find_t *)arg;
    if (e->nlen != f->wlen) return 0;
    for (int i = 0; i < e->nlen; i++)
        if (ex_up(v, e->name[i]) != ex_up(v, f->want[i])) return 0;
    *f->found = *e;
    return 1;
}

/* Resolve @path (relative to the volume root, '/'-separated). On success
 * fills @out (for "" / root: a synthetic directory entry). */
static int ex_lookup(exfat_vol_t *v, const char *path, ex_ent_t *out) {
    memset(out, 0, sizeof(*out));
    out->attr = EX_ATTR_DIR;
    out->first = v->root;
    out->flags = 0x01;
    out->len = 0;                           /* root: chain-terminated */
    out->is_root = true;
    static uint16_t comp[256];
    /* normalise: drop "." components, ".." pops (never above the volume root) */
    static char norm[512];
    {
        int o = 0, starts[64], depth = 0;
        const char *q = path;
        while (*q) {
            while (*q == '/') q++;
            const char *s0 = q;
            while (*q && *q != '/') q++;
            int n = (int)(q - s0);
            if (n == 0 || (n == 1 && s0[0] == '.')) continue;
            if (n == 2 && s0[0] == '.' && s0[1] == '.') {
                if (depth > 0) o = starts[--depth];
                continue;
            }
            if (depth >= 64 || o + n + 2 >= (int)sizeof(norm)) return -1;
            starts[depth++] = o;
            if (o) norm[o++] = '/';
            memcpy(norm + o, s0, n); o += n;
        }
        norm[o] = 0;
    }
    if (ex_debug) serial_printf("[exfat] lookup '%s' -> '%s'\n", path, norm);
    const char *p = norm;
    while (*p == '/') p++;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/') p++;
        int n = (int)(p - s);
        while (*p == '/') p++;
        if (n == 0 || (n == 1 && s[0] == '.')) continue;
        if (!(out->attr & EX_ATTR_DIR)) return -1;
        int wl = ex_utf8_to16(s, n, comp, 255);
        if (wl <= 0) return -1;
        ex_run_t dir = { out->first, out->flags, out->len };
        ex_ent_t *found = (ex_ent_t *)kalloc(sizeof(ex_ent_t));
        if (!found) return -1;
        ex_find_t f = { comp, wl, found };
        int rc = ex_walk_dir(v, &dir, ex_find_cb, &f);
        if (rc != 1) { kfree(found); return -1; }
        *out = *found;
        kfree(found);
    }
    return 0;
}

/* ---- mount ----------------------------------------------------------------- */

bool exfat_probe(uint8_t dev, uint32_t lba) {
    static uint8_t bs[512];
    if (storage_read_sectors(dev, lba, 1, bs) != 0) return false;
    return memcmp(bs + 3, "EXFAT   ", 8) == 0 && bs[510] == 0x55 && bs[511] == 0xAA;
}

int64_t exfat_find_partition(uint8_t dev) {
    uint32_t st[17];
    int n = partscan_starts(dev, st, 17);
    for (int i = 0; i < n; i++)
        if (exfat_probe(dev, st[i])) return st[i];
    return -1;
}

static int ex_boot_checksum_ok(uint8_t dev, uint32_t part) {
    uint8_t *b = (uint8_t *)kalloc(12 * 512);
    if (!b) return -1;
    if (storage_read_sectors(dev, part, 12, b) != 0) { kfree(b); return -1; }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 11 * 512; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + b[i];
    }
    int ok = 1;
    for (int k = 0; k < 128; k++)
        if (((uint32_t *)(b + 11 * 512))[k] != sum) { ok = 0; break; }
    kfree(b);
    return ok;
}

typedef struct { exfat_vol_t *v; int seen; uint32_t up_clus; uint64_t up_len; } ex_meta_t;

/* Root-only special entries: 0x81 bitmap, 0x82 upcase, 0x83 label. */
static int ex_scan_meta(exfat_vol_t *v, ex_meta_t *m) {
    static uint8_t sec[512];
    uint32_t c = v->root;
    for (int guard = 0; ex_valid_clus(v, c) && guard < 64; guard++) {
        for (uint32_t s = 0; s < v->spc; s++) {
            if (storage_read_sectors(v->dev, ex_lba(v, c) + s, 1, sec) != 0) return -1;
            for (uint32_t o = 0; o < 512; o += 32) {
                uint8_t t = sec[o];
                if (t == 0) return 0;
                if (t == 0x81 && !(sec[o + 1] & 1)) {
                    v->bitmap_clus = *(uint32_t *)(sec + o + 20);
                    v->bitmap_len = *(uint64_t *)(sec + o + 24);
                    m->seen |= 1;
                } else if (t == 0x82) {
                    m->up_clus = *(uint32_t *)(sec + o + 20);
                    m->up_len = *(uint64_t *)(sec + o + 24);
                    m->seen |= 2;
                } else if (t == 0x83) {
                    int n = sec[o + 1];
                    if (n > 11) n = 11;
                    ex_ent_t tmp; tmp.nlen = n;
                    for (int k = 0; k < n; k++) tmp.name[k] = *(uint16_t *)(sec + o + 2 + 2 * k);
                    ex_name_utf8(&tmp, v->label, sizeof(v->label));
                }
            }
        }
        c = ex_fat_next(v, c);
    }
    return 0;
}

static int ex_load_upcase(exfat_vol_t *v, uint32_t clus, uint64_t len) {
    v->upcase = (uint16_t *)kalloc(65536 * sizeof(uint16_t));
    if (!v->upcase) return -1;
    for (uint32_t i = 0; i < 65536; i++) v->upcase[i] = (uint16_t)i;
    if (!clus || len == 0 || len > 256 * 1024) return 0;
    uint8_t *raw = (uint8_t *)kalloc((uint32_t)len + 512);
    if (!raw) return 0;
    ex_run_t r = { clus, 0x01, len };
    uint32_t ci = 0, cc = 0, got = 0;
    for (uint32_t idx = 0; got < len; idx++) {
        uint32_t c = ex_run_clus(v, &r, idx, &ci, &cc);
        if (!ex_valid_clus(v, c)) break;
        for (uint32_t s = 0; s < v->spc && got < len; s++, got += 512)
            if (storage_read_sectors(v->dev, ex_lba(v, c) + s, 1, raw + got) != 0) { kfree(raw); return 0; }
    }
    /* compressed form: 0xFFFF <count> = identity run */
    uint32_t n = (uint32_t)(len / 2), ch = 0;
    const uint16_t *u = (const uint16_t *)raw;
    for (uint32_t i = 0; i < n && ch < 65536; i++) {
        if (u[i] == 0xFFFF && i + 1 < n) { ch += u[i + 1]; i++; continue; }
        v->upcase[ch++] = u[i];
    }
    kfree(raw);
    return 0;
}

exfat_vol_t *exfat_mount(uint8_t dev, uint32_t part) {
    static uint8_t bs[512];
    if (storage_read_sectors(dev, part, 1, bs) != 0 || memcmp(bs + 3, "EXFAT   ", 8) != 0) {
        printf("exfat: no exFAT boot sector on device %u at LBA %u\n", dev, part);
        return NULL;
    }
    if (bs[108] != 9) {
        printf("exfat: %u-byte sectors not supported (need 512)\n", 1u << bs[108]);
        return NULL;
    }
    if (bs[109] > 16) { printf("exfat: cluster size too large\n"); return NULL; }
    int ck = ex_boot_checksum_ok(dev, part);
    if (ck == 0) { printf("exfat: boot region checksum mismatch - not mounting\n"); return NULL; }
    exfat_vol_t *v = (exfat_vol_t *)kalloc(sizeof(exfat_vol_t));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->dev = dev; v->part = part;
    v->fat_off = *(uint32_t *)(bs + 80);
    v->fat_len = *(uint32_t *)(bs + 84);
    v->heap_off = *(uint32_t *)(bs + 88);
    v->clus_count = *(uint32_t *)(bs + 92);
    v->root = *(uint32_t *)(bs + 96);
    v->spc_shift = bs[109];
    v->spc = 1u << bs[109];
    v->cbytes = v->spc * 512u;
    v->fat_cached = 0xFFFFFFFFu;
    ex_meta_t m = { v, 0, 0, 0 };
    if (ex_scan_meta(v, &m) != 0 || !(m.seen & 1)) {
        printf("exfat: root directory has no allocation bitmap - not mounting\n");
        kfree(v);
        return NULL;
    }
    ex_load_upcase(v, m.up_clus, m.up_len);
    serial_printf("[exfat] mounted dev %u LBA %u label '%s': %u clusters of %u KiB, root cluster %u\n",
                  dev, part, v->label, v->clus_count, v->cbytes / 1024, v->root);
    return v;
}

void exfat_unmount(exfat_vol_t *v) {
    if (!v) return;
    for (int i = 0; i < EX_MAX_VOLS; i++) if (ex_vols[i] == v) ex_vols[i] = NULL;
    if (v->rw) {
        storage_flush(v->dev);
        if (v->dirty_on) ex_set_volflags(v, false);   /* clean unmount */
        serial_printf("[exfat] '%s' unmounted cleanly (VolumeDirty cleared)\n", v->label);
        if (v->bm) kfree(v->bm);
    }
    if (v->upcase) kfree(v->upcase);
    kfree(v);
}

void exfat_info(const exfat_vol_t *v) {
    if (!v) return;
    printf("exFAT '%s' on device %u at LBA %u: %u clusters x %u KiB (%s)\n",
           v->label, v->dev, v->part, v->clus_count, v->cbytes / 1024, v->rw ? "read-write" : "read-only");
}


/* ---- write support (only when mounted with exfat_enable_rw) ------------------ */

static uint32_t ex_now(void) {
    rtc_time_t t; rtc_read(&t);
    uint32_t y = t.year >= 1980 ? t.year - 1980u : 0;
    return (y << 25) | ((uint32_t)t.month << 21) | ((uint32_t)t.day << 16) |
           ((uint32_t)t.hour << 11) | ((uint32_t)t.minute << 5) | (t.second >> 1);
}

static uint16_t ex_rot(uint16_t x, uint8_t b) { return (uint16_t)(((x << 15) | (x >> 1)) + b); }

/* Byte-range IO inside a directory run (sector read-modify-write). */
static int ex_dir_io(exfat_vol_t *v, const ex_run_t *dir, uint64_t off, uint8_t *buf, uint32_t len, int wr) {
    static uint8_t sec[512];
    uint32_t ci = 0, cc = 0;
    while (len) {
        uint32_t idx = (uint32_t)(off / v->cbytes), in = (uint32_t)(off % v->cbytes);
        uint32_t c = ex_run_clus(v, dir, idx, &ci, &cc);
        if (!ex_valid_clus(v, c)) return -1;
        uint32_t lba = ex_lba(v, c) + in / 512u, so = in % 512u, n = 512u - so;
        if (n > len) n = len;
        if (storage_read_sectors(v->dev, lba, 1, sec) != 0) return -1;
        if (wr) {
            memcpy(sec + so, buf, n);
            if (storage_write_sectors(v->dev, lba, 1, sec) != 0) return -1;
        } else memcpy(buf, sec + so, n);
        buf += n; off += n; len -= n;
    }
    return 0;
}

static void ex_set_checksum(uint8_t *set, int n) {
    uint16_t ck = 0;
    for (int i = 0; i < n * 32; i++) { if (i == 2 || i == 3) continue; ck = ex_rot(ck, set[i]); }
    *(uint16_t *)(set + 2) = ck;
}

/* Rewrite the file + stream entries of @e's set from @e. */
static int ex_set_write(exfat_vol_t *v, const ex_ent_t *e) {
    if (e->is_root) return 0;
    uint8_t set[19 * 32];
    int n = 1 + e->nsec;
    if (n < 2 || n > 19) return -1;
    if (ex_dir_io(v, &e->parent, e->set_off, set, n * 32, 0) != 0) return -1;
    if (set[0] != 0x85 || set[32] != 0xC0) return -1;
    *(uint16_t *)(set + 4) = e->attr;
    *(uint32_t *)(set + 8) = e->ctime;
    *(uint32_t *)(set + 12) = e->mtime;
    *(uint32_t *)(set + 16) = e->atime;
    set[33] = e->flags;
    *(uint64_t *)(set + 40) = e->vdl;
    *(uint32_t *)(set + 52) = e->first;
    *(uint64_t *)(set + 56) = e->len;
    ex_set_checksum(set, n);
    return ex_dir_io(v, &e->parent, e->set_off, set, n * 32, 1);
}

static bool ex_bm_get(exfat_vol_t *v, uint32_t c) { uint32_t b = c - 2; return (v->bm[b >> 3] >> (b & 7)) & 1; }

static int ex_bm_put(exfat_vol_t *v, uint32_t c, bool on) {
    uint32_t b = c - 2;
    if (on) v->bm[b >> 3] |= (uint8_t)(1u << (b & 7)); else v->bm[b >> 3] &= (uint8_t)~(1u << (b & 7));
    uint32_t byte = b >> 3, sidx = byte / 512u;
    ex_run_t r = { v->bitmap_clus, 0x01, 0 };
    uint32_t c2 = ex_run_clus(v, &r, sidx / v->spc, NULL, NULL);
    if (!ex_valid_clus(v, c2)) return -1;
    return storage_write_sectors(v->dev, ex_lba(v, c2) + sidx % v->spc, 1, v->bm + sidx * 512u);
}

static int ex_fat_set(exfat_vol_t *v, uint32_t c, uint32_t val) {
    uint32_t sec = v->part + v->fat_off + (c * 4u) / 512u;
    if (v->fat_cached != sec) {
        if (storage_read_sectors(v->dev, sec, 1, v->fat_sec) != 0) return -1;
        v->fat_cached = sec;
    }
    *(uint32_t *)(v->fat_sec + (c * 4u) % 512u) = val;
    return storage_write_sectors(v->dev, sec, 1, v->fat_sec);
}

/* Allocate one cluster, preferring @want. Returns 0 when full. */
static uint32_t ex_alloc(exfat_vol_t *v, uint32_t want) {
    uint32_t c = 0;
    if (ex_valid_clus(v, want) && !ex_bm_get(v, want)) c = want;
    for (uint32_t i = 0; !c && i < v->clus_count; i++) {
        uint32_t k = 2 + (v->bm_hint + i) % v->clus_count;
        if (!ex_bm_get(v, k)) c = k;
    }
    if (!c) return 0;
    if (ex_bm_put(v, c, true) != 0) return 0;
    v->bm_hint = c - 1;            /* next search starts after c */
    return c;
}

static uint32_t ex_nclus(exfat_vol_t *v, uint64_t len) { return (uint32_t)((len + v->cbytes - 1) / v->cbytes); }

/* Grow @e's allocation from @have to @want clusters. @last = current last
 * cluster (0 if none). Keeps NoFatChain while contiguous, converts the
 * existing run to a FAT chain on the first non-adjacent cluster. */
static int ex_grow(exfat_vol_t *v, ex_ent_t *e, uint32_t *havep, uint32_t want, uint32_t *last) {
    uint32_t have = *havep;
    while (have < want) {
        uint32_t c;
        if (have == 0) {
            /* start new files where at least two clusters are free, so a
             * growing file does not fragment on its second cluster */
            uint32_t pick = 0;
            for (uint32_t i = 0; !pick && i < v->clus_count; i++) {
                uint32_t k = 2 + (v->bm_hint + i) % v->clus_count;
                if (!ex_bm_get(v, k) && ex_valid_clus(v, k + 1) && !ex_bm_get(v, k + 1)) pick = k;
            }
            c = ex_alloc(v, pick);
            if (!c) return -1;
            e->first = c; e->flags = 0x03;          /* alloc possible + NoFatChain */
        } else {
            c = ex_alloc(v, *last + 1);
            if (!c) return -1;
            if ((e->flags & EX_NOFATCHAIN) && c != *last + 1) {
                for (uint32_t j = 0; j < have; j++)
                    if (ex_fat_set(v, e->first + j, j + 1 < have ? e->first + j + 1 : EX_EOC) != 0) return -1;
                e->flags &= (uint8_t)~EX_NOFATCHAIN;
                serial_printf("[exfat] fragmented: converted run at %u (%u clusters) to FAT chain\n", e->first, have);
            }
            if (!(e->flags & EX_NOFATCHAIN) && ex_fat_set(v, *last, c) != 0) return -1;
        }
        if (!(e->flags & EX_NOFATCHAIN) && ex_fat_set(v, c, EX_EOC) != 0) return -1;
        *last = c; have++; *havep = have;
    }
    return 0;
}

/* Keep the first @keep of @cur clusters, free the rest. Updates e->first/flags
 * and *last. Does not touch len/vdl. */
static void ex_free_run(exfat_vol_t *v, uint32_t first, uint8_t flags, uint32_t n);
static void ex_shrink(exfat_vol_t *v, ex_ent_t *e, uint32_t keep, uint32_t cur, uint32_t *last) {
    if (keep >= cur) return;
    if (keep == 0) {
        ex_free_run(v, e->first, e->flags, cur);
        e->first = 0; e->flags = 0x01; *last = 0;
        return;
    }
    if (e->flags & EX_NOFATCHAIN) {
        for (uint32_t i = keep; i < cur; i++) ex_bm_put(v, e->first + i, false);
        *last = e->first + keep - 1;
        return;
    }
    ex_run_t r = { e->first, e->flags, 0 };
    uint32_t c = ex_run_clus(v, &r, keep - 1, NULL, NULL);
    if (!ex_valid_clus(v, c)) return;
    uint32_t next = ex_fat_next(v, c);
    ex_fat_set(v, c, EX_EOC);
    if (ex_valid_clus(v, next)) ex_free_run(v, next, e->flags, cur - keep);
    *last = c;
}

static void ex_free_run(exfat_vol_t *v, uint32_t first, uint8_t flags, uint32_t n) {
    uint32_t c = first;
    for (uint32_t i = 0; i < n && ex_valid_clus(v, c); i++) {
        uint32_t next = (flags & EX_NOFATCHAIN) ? c + 1 : ex_fat_next(v, c);
        ex_bm_put(v, c, false);
        c = next;
    }
}

static int ex_zero_clus(exfat_vol_t *v, uint32_t c) {
    static uint8_t z[512];
    memset(z, 0, sizeof(z));
    for (uint32_t s = 0; s < v->spc; s++)
        if (storage_write_sectors(v->dev, ex_lba(v, c) + s, 1, z) != 0) return -1;
    return 0;
}

static int ex_set_volflags(exfat_vol_t *v, bool dirty) {
    static uint8_t bs[512];
    if (storage_read_sectors(v->dev, v->part, 1, bs) != 0) return -1;
    uint16_t f = *(uint16_t *)(bs + 106);
    f = dirty ? (uint16_t)(f | 2) : (uint16_t)(f & ~2);
    *(uint16_t *)(bs + 106) = f;                /* excluded from the boot checksum */
    int r = storage_write_sectors(v->dev, v->part, 1, bs);
    storage_flush(v->dev);
    return r;
}

static int ex_mark_dirty(exfat_vol_t *v) {
    if (!v->rw) return -1;
    if (v->dirty_on) return 0;
    if (ex_set_volflags(v, true) != 0) return -1;
    v->dirty_on = true;
    return 0;
}

int exfat_enable_rw(exfat_vol_t *v) {
    if (!v || v->rw) return v ? 0 : -1;
    uint32_t bytes = ex_nclus(v, v->bitmap_len) * v->cbytes;
    if (v->bitmap_len < (v->clus_count + 7) / 8 || bytes == 0) return -1;
    v->bm = (uint8_t *)kalloc(bytes);
    if (!v->bm) return -1;
    memset(v->bm, 0, bytes);
    ex_run_t r = { v->bitmap_clus, 0x01, 0 };
    for (uint32_t i = 0; i < bytes / v->cbytes; i++) {
        uint32_t c = ex_run_clus(v, &r, i, NULL, NULL);
        if (!ex_valid_clus(v, c) ||
            storage_read_sectors(v->dev, ex_lba(v, c), v->spc, v->bm + i * v->cbytes) != 0) {
            kfree(v->bm); v->bm = NULL; return -1;
        }
    }
    if (ex_set_volflags(v, true) != 0) { kfree(v->bm); v->bm = NULL; return -1; }
    v->rw = true; v->dirty_on = true;
    serial_printf("[exfat] '%s' now read-write (VolumeDirty set)\n", v->label);
    return 0;
}

static int ex_rw_handles(exfat_vol_t *v);
int exfat_sync(exfat_vol_t *v) {
    if (!v || !v->rw) return 0;
    int r = storage_flush(v->dev);
    if (r == 0 && v->dirty_on && ex_rw_handles(v) == 0 && ex_set_volflags(v, false) == 0) {
        v->dirty_on = false;                     /* set again by the next change */
        serial_printf("[exfat] '%s' synced, VolumeDirty cleared\n", v->label);
    }
    return r;
}

/* Split "a/b/c" into parent "a/b" and name "c". */
static int ex_split(const char *rel, char *parent, int pcap, const char **name) {
    int n = (int)strlen(rel);
    while (n > 0 && rel[n - 1] == '/') n--;
    int k = n;
    while (k > 0 && rel[k - 1] != '/') k--;
    if (k == n) return -1;                      /* empty name */
    int pl = k > 0 ? k - 1 : 0;
    if (pl >= pcap) return -1;
    memcpy(parent, rel, pl); parent[pl] = 0;
    *name = rel + k;
    return n - k;                               /* name length */
}

/* Directory capacity in bytes (root: walk its FAT chain). Sets @lastc. */
static uint64_t ex_dir_cap(exfat_vol_t *v, const ex_ent_t *d, uint32_t *lastc) {
    if (!d->is_root) {
        uint32_t n = ex_nclus(v, d->len), c = d->first;
        if (d->flags & EX_NOFATCHAIN) *lastc = d->first + n - 1;
        else { for (uint32_t i = 1; i < n; i++) c = ex_fat_next(v, c); *lastc = c; }
        return d->len;
    }
    uint32_t c = v->root, n = 1;
    for (;;) { uint32_t x = ex_fat_next(v, c); if (!ex_valid_clus(v, x) || n > 1u << 16) break; c = x; n++; }
    *lastc = c;
    return (uint64_t)n * v->cbytes;
}

/* Create an entry set for @rel. @attr 0x10 = directory (gets one zeroed cluster). */
static int ex_create_entry(exfat_vol_t *v, const char *rel, uint16_t attr, const ex_ent_t *tmpl) {
    static char parent[512];
    static uint16_t u[256];
    const char *nm;
    int nl8 = ex_split(rel, parent, sizeof(parent), &nm);
    if (nl8 <= 0) return -1;
    int nl = ex_utf8_to16(nm, nl8, u, 255);
    if (nl <= 0) return -1;
    ex_ent_t *d = (ex_ent_t *)kalloc(sizeof(ex_ent_t) * 2);
    if (!d) return -1;
    ex_ent_t *tmp = d + 1;
    int rc = -1;
    if (ex_lookup(v, parent, d) != 0 || !(d->attr & EX_ATTR_DIR)) goto out;
    if (ex_lookup(v, rel, tmp) == 0) goto out;               /* exists */
    int nn = (nl + 14) / 15, total = 2 + nn;
    uint8_t set[19 * 32];
    memset(set, 0, sizeof(set));
    uint32_t now = ex_now();
    set[0] = 0x85; set[1] = (uint8_t)(1 + nn);
    *(uint16_t *)(set + 4) = attr;
    *(uint32_t *)(set + 8) = now; *(uint32_t *)(set + 12) = now; *(uint32_t *)(set + 16) = now;
    set[32] = 0xC0; set[33] = 0x01; set[35] = (uint8_t)nl;
    uint16_t h = 0;
    for (int i = 0; i < nl; i++) { uint16_t c = ex_up(v, u[i]); h = ex_rot(h, c & 0xFF); h = ex_rot(h, c >> 8); }
    *(uint16_t *)(set + 36) = h;
    for (int i = 0; i < nn; i++) {
        set[64 + 32 * i] = 0xC1;
        for (int k = 0; k < 15 && i * 15 + k < nl; k++)
            *(uint16_t *)(set + 64 + 32 * i + 2 + 2 * k) = u[i * 15 + k];
    }
    if (tmpl) {                                             /* rename: move data as-is */
        *(uint16_t *)(set + 4) = tmpl->attr;
        *(uint32_t *)(set + 8) = tmpl->ctime; *(uint32_t *)(set + 12) = tmpl->mtime;
        *(uint32_t *)(set + 16) = tmpl->atime;
        set[33] = tmpl->flags;
        *(uint64_t *)(set + 40) = tmpl->vdl;
        *(uint32_t *)(set + 52) = tmpl->first;
        *(uint64_t *)(set + 56) = tmpl->len;
    } else if (attr & EX_ATTR_DIR) {
        uint32_t c = ex_alloc(v, 0);
        if (!c || ex_zero_clus(v, c) != 0) goto out;
        set[33] = 0x03;
        *(uint64_t *)(set + 40) = v->cbytes;
        *(uint32_t *)(set + 52) = c;
        *(uint64_t *)(set + 56) = v->cbytes;
    }
    ex_set_checksum(set, total);
    /* find @total consecutive free entries */
    ex_run_t dr = { d->first, d->flags, d->is_root ? 0 : d->len };
    uint32_t lastc = 0;
    uint64_t cap = ex_dir_cap(v, d, &lastc), runst = 0, slot = (uint64_t)-1;
    int run = 0;
    uint8_t ent[32];
    for (uint64_t o = 0; o < cap; o += 32) {
        if (ex_dir_io(v, &dr, o, ent, 1, 0) != 0) goto out;
        if (ent[0] & 0x80) { run = 0; continue; }
        if (run == 0) runst = o;
        if (++run == total) { slot = runst; break; }
    }
    if (slot == (uint64_t)-1) {                             /* extend the directory */
        uint32_t c = ex_alloc(v, lastc + 1);
        if (!c || ex_zero_clus(v, c) != 0) goto out;
        if (d->is_root) {
            if (ex_fat_set(v, lastc, c) != 0 || ex_fat_set(v, c, EX_EOC) != 0) goto out;
        } else {
            ex_bm_put(v, c, false);                         /* ex_grow allocates again */
            uint32_t have = ex_nclus(v, d->len), last = lastc;
            if (ex_grow(v, d, &have, have + 1, &last) != 0) goto out;
            if (ex_zero_clus(v, last) != 0) goto out;
            d->len += v->cbytes; d->vdl = d->len;
            d->mtime = now;
            if (ex_set_write(v, d) != 0) goto out;
        }
        slot = run ? runst : cap;
        dr.first = d->first; dr.flags = d->flags; dr.len = d->is_root ? 0 : d->len;
    }
    if (ex_dir_io(v, &dr, slot, set, total * 32, 1) != 0) goto out;
    rc = 0;
out:
    kfree(d);
    return rc;
}

static int ex_any_cb(exfat_vol_t *v, const ex_ent_t *e, void *arg) { return 1; }

static int ex_unlink_entry(exfat_vol_t *v, const char *rel, bool want_dir) {
    ex_ent_t *e = (ex_ent_t *)kalloc(sizeof(ex_ent_t));
    if (!e) return -1;
    int rc = -1;
    if (ex_lookup(v, rel, e) != 0 || e->is_root) goto out;
    if (!!(e->attr & EX_ATTR_DIR) != want_dir) goto out;
    if (want_dir) {                                         /* must be empty */
        ex_run_t dr = { e->first, e->flags, e->len };
        if (ex_walk_dir(v, &dr, ex_any_cb, NULL) != 0) goto out;
    }
    if (e->len) ex_free_run(v, e->first, e->flags, ex_nclus(v, e->len));
    uint8_t set[19 * 32];
    int n = 1 + e->nsec;
    if (n < 2 || n > 19 || ex_dir_io(v, &e->parent, e->set_off, set, n * 32, 0) != 0) goto out;
    for (int i = 0; i < n; i++) set[i * 32] &= 0x7F;
    rc = ex_dir_io(v, &e->parent, e->set_off, set, n * 32, 1);
out:
    kfree(e);
    return rc;
}

/* ---- VFS adapter ----------------------------------------------------------- */

#define EX_MAX_HANDLES 64
static int ex_nopen = 0, ex_nclose = 0, ex_nfail = 0, ex_nmax = 0;
typedef struct {
    bool in_use;
    exfat_vol_t *v;
    ex_run_t run;
    uint64_t vdl;
    uint64_t pos;
    uint32_t ci, cc;                 /* cluster walk cache */
    bool wr, append;
    uint32_t nclus, last;            /* allocated clusters, last cluster */
    ex_ent_t e;                      /* entry (with set location) for writes */
} ex_handle_t;
static ex_handle_t ex_handles[EX_MAX_HANDLES];

static void ex_h_sync(ex_handle_t *h) {
    h->run.first = h->e.first; h->run.flags = h->e.flags; h->run.len = h->e.len;
    h->vdl = h->e.vdl < h->e.len ? h->e.vdl : h->e.len;
    h->ci = h->cc = 0;
}

/* Same file = same volume + same entry-set location. */
static bool ex_same_file(const ex_handle_t *a, exfat_vol_t *v, const ex_ent_t *e) {
    return a->in_use && a->v == v && !e->is_root && a->e.parent.first == e->parent.first &&
           a->e.set_off == e->set_off;
}

/* Propagate size/allocation of @e to every open handle of that file (except @self). */
static void ex_share(exfat_vol_t *v, const ex_ent_t *e, uint32_t nclus, uint32_t last, const ex_handle_t *self) {
    for (int i = 0; i < EX_MAX_HANDLES; i++) {
        ex_handle_t *o = &ex_handles[i];
        if (o == self || !ex_same_file(o, v, e)) continue;
        o->e.first = e->first; o->e.flags = e->flags; o->e.len = e->len; o->e.vdl = e->vdl;
        o->e.mtime = e->mtime; o->e.atime = e->atime; o->e.attr = e->attr;
        o->nclus = nclus; o->last = last;
        ex_h_sync(o);
    }
}

static int ex_rw_handles(exfat_vol_t *v) {
    int n = 0;
    for (int i = 0; i < EX_MAX_HANDLES; i++) if (ex_handles[i].in_use && ex_handles[i].v == v && ex_handles[i].wr) n++;
    return n;
}

/* Set the size of @e (clusters in *nclus, last cluster in *last). Growing keeps
 * ValidDataLength, so the new tail reads as zeros; shrinking frees clusters. */
static int ex_resize(exfat_vol_t *v, ex_ent_t *e, uint64_t len, uint32_t *nclus, uint32_t *last) {
    uint32_t want = ex_nclus(v, len);
    if (want > *nclus) {
        uint32_t orig = *nclus, olast = *last;
        uint8_t oflags = e->flags; uint32_t ofirst = e->first;
        if (ex_grow(v, e, nclus, want, last) != 0) {
            ex_shrink(v, e, orig, *nclus, last);         /* give back what we took */
            *nclus = orig;
            if (orig == 0) { e->first = ofirst; e->flags = oflags; *last = olast; }
            return -1;
        }
    } else if (want < *nclus) {
        ex_shrink(v, e, want, *nclus, last);
        *nclus = want;
    }
    e->len = len;
    if (e->vdl > len) e->vdl = len;
    if (len == 0) { e->first = 0; e->flags = 0x01; }
    e->mtime = e->atime = ex_now();
    return ex_set_write(v, e);
}

static void *ex_open(vfs_filesystem_t *fs, const char *rel, uint32_t flags) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    bool wr = (flags & (VFS_O_WRITE | VFS_O_TRUNC | VFS_O_APPEND)) != 0;
    if (wr && (!v->rw || ex_mark_dirty(v) != 0)) return NULL;   /* read-only mount */
    int slot = -1;
    for (int i = 0; i < EX_MAX_HANDLES; i++) if (!ex_handles[i].in_use) { slot = i; break; }
    if (slot < 0) { ex_nfail++; serial_printf("[exfat] OPEN FAIL active=%d\n", ex_nopen - ex_nclose); return NULL; }
    ex_handle_t *h = &ex_handles[slot];
    memset(h, 0, sizeof(*h));
    if (ex_lookup(v, rel, &h->e) != 0 || (h->e.attr & EX_ATTR_DIR)) return NULL;
    h->in_use = true; h->v = v; ex_nopen++; if (ex_nopen - ex_nclose > ex_nmax) ex_nmax = ex_nopen - ex_nclose;
    h->wr = wr; h->append = (flags & VFS_O_APPEND) != 0;
    h->nclus = ex_nclus(v, h->e.len);
    h->last = 0;
    if (h->nclus) {
        if (h->e.flags & EX_NOFATCHAIN) h->last = h->e.first + h->nclus - 1;
        else { ex_run_t r = { h->e.first, h->e.flags, h->e.len };
               h->last = ex_run_clus(v, &r, h->nclus - 1, NULL, NULL); }
    }
    if (wr && (flags & VFS_O_TRUNC) && h->e.len) {
        ex_free_run(v, h->e.first, h->e.flags, h->nclus);
        h->e.first = 0; h->e.len = h->e.vdl = 0; h->e.flags = 0x01;
        h->e.mtime = h->e.atime = ex_now();
        h->nclus = 0; h->last = 0;
        ex_set_write(v, &h->e);
        ex_share(v, &h->e, 0, 0, h);
    }
    ex_h_sync(h);
    if (h->append) h->pos = h->e.len;
    return (void *)(uintptr_t)(slot + 1);
}

static ex_handle_t *ex_h(void *handle) {
    int s = (int)(uintptr_t)handle - 1;
    if (s < 0 || s >= EX_MAX_HANDLES || !ex_handles[s].in_use) return NULL;
    return &ex_handles[s];
}

static int ex_close(vfs_filesystem_t *fs, void *handle) {
    ex_handle_t *h = ex_h(handle);
    if (!h) return -1;
    h->in_use = false; ex_nclose++;
    if (ex_debug)
        serial_printf("[exfat] handles open=%d close=%d active=%d max=%d fail=%d\n",
                      ex_nopen, ex_nclose, ex_nopen - ex_nclose, ex_nmax, ex_nfail);
    return 0;
}

static int ex_read(vfs_filesystem_t *fs, void *handle, void *buffer, uint32_t size) {
    ex_handle_t *h = ex_h(handle);
    if (!h) return -1;
    exfat_vol_t *v = h->v;
    if (h->pos >= h->run.len) return 0;
    if (size > h->run.len - h->pos) size = (uint32_t)(h->run.len - h->pos);
    uint8_t *dst = (uint8_t *)buffer;
    static uint8_t bounce[512];
    uint32_t done = 0;
    while (done < size) {
        uint64_t pos = h->pos;
        if (pos >= h->vdl) {                         /* beyond valid data: zeros */
            uint32_t n = size - done;
            memset(dst + done, 0, n);
            done += n; h->pos += n;
            break;
        }
        uint32_t idx = (uint32_t)(pos / v->cbytes);
        uint32_t in = (uint32_t)(pos % v->cbytes);
        uint32_t c = ex_run_clus(v, &h->run, idx, &h->ci, &h->cc);
        if (!ex_valid_clus(v, c)) return done ? (int)done : -1;
        uint64_t lim = h->vdl - pos;
        uint32_t want = size - done;
        if (want > lim) want = (uint32_t)lim;
        if (want > v->cbytes - in) want = v->cbytes - in;
        uint32_t lba = ex_lba(v, c) + in / 512u, so = in % 512u;
        if (so == 0 && want >= 512) {
            uint32_t ns = want / 512u;
            if (storage_read_sectors(v->dev, lba, ns, dst + done) != 0) return done ? (int)done : -1;
            want = ns * 512u;
        } else {
            if (storage_read_sectors(v->dev, lba, 1, bounce) != 0) return done ? (int)done : -1;
            if (want > 512 - so) want = 512 - so;
            memcpy(dst + done, bounce + so, want);
        }
        done += want; h->pos += want;
    }
    return (int)done;
}

static int ex_write_data(ex_handle_t *h, uint64_t pos, const uint8_t *src, uint32_t size) {
    exfat_vol_t *v = h->v;
    static uint8_t bounce[512];
    uint32_t done = 0;
    while (done < size) {
        uint64_t p = pos + done;
        uint32_t idx = (uint32_t)(p / v->cbytes), in = (uint32_t)(p % v->cbytes);
        uint32_t c = ex_run_clus(v, &h->run, idx, &h->ci, &h->cc);
        if (!ex_valid_clus(v, c)) return -1;
        uint32_t want = size - done;
        if (want > v->cbytes - in) want = v->cbytes - in;
        uint32_t lba = ex_lba(v, c) + in / 512u, so = in % 512u;
        if (so == 0 && want >= 512 && src) {
            uint32_t ns = want / 512u;
            if (storage_write_sectors(v->dev, lba, ns, src + done) != 0) return -1;
            want = ns * 512u;
        } else {
            if (want > 512 - so) want = 512 - so;
            if (so != 0 || want < 512) { if (storage_read_sectors(v->dev, lba, 1, bounce) != 0) return -1; }
            if (src) memcpy(bounce + so, src + done, want); else memset(bounce + so, 0, want);
            if (storage_write_sectors(v->dev, lba, 1, bounce) != 0) return -1;
        }
        done += want;
    }
    return 0;
}

static int ex_write(vfs_filesystem_t *fs, void *handle, const void *buffer, uint32_t size) {
    ex_handle_t *h = ex_h(handle);
    if (!h || !h->wr || !h->v->rw) return -1;
    exfat_vol_t *v = h->v;
    if (h->append) h->pos = h->e.len;
    if (size == 0) return 0;
    uint64_t end = h->pos + size;
    uint32_t need = ex_nclus(v, end);
    if (need > h->nclus) {
        uint32_t orig = h->nclus;
        int g = ex_grow(v, &h->e, &h->nclus, need, &h->last);
        if (g != 0) {
            ex_shrink(v, &h->e, orig, h->nclus, &h->last);   /* release partial allocation */
            h->nclus = orig;
            if (orig == 0) { h->e.first = 0; h->e.flags = 0x01; }
            ex_set_write(v, &h->e);
            ex_h_sync(h);
            serial_printf("[exfat] write: volume full - released partial allocation\n");
            return -1;
        }
        h->run.first = h->e.first; h->run.flags = h->e.flags; h->ci = h->cc = 0;
    }
    /* zero the gap between valid data and the write position */
    if (h->pos > h->e.vdl) {
        for (uint64_t z = h->e.vdl; z < h->pos; ) {
            uint32_t n = (uint32_t)((h->pos - z) > 65536 ? 65536 : (h->pos - z));
            if (ex_write_data(h, z, NULL, n) != 0) return -1;
            z += n;
        }
    }
    if (ex_write_data(h, h->pos, (const uint8_t *)buffer, size) != 0) return -1;
    h->pos = end;
    if (end > h->e.len) h->e.len = end;
    if (end > h->e.vdl) h->e.vdl = end;
    h->e.attr |= 0x20;
    h->e.mtime = h->e.atime = ex_now();
    if (ex_set_write(v, &h->e) != 0) return -1;
    ex_h_sync(h);
    ex_share(v, &h->e, h->nclus, h->last, h);
    return (int)size;
}

static int ex_seek(vfs_filesystem_t *fs, void *handle, int32_t off, int whence) {
    ex_handle_t *h = ex_h(handle);
    if (!h) return -1;
    int64_t np = whence == VFS_SEEK_SET ? off : whence == VFS_SEEK_CUR ? (int64_t)h->pos + off
               : whence == VFS_SEEK_END ? (int64_t)h->run.len + off : -1;
    if (np < 0) return -1;
    h->pos = (uint64_t)np;
    return (int)np;
}

static int ex_get_size(vfs_filesystem_t *fs, void *handle) {
    ex_handle_t *h = ex_h(handle);
    return h ? (int)h->run.len : -1;
}

typedef struct { vfs_dirent_t *out; uint32_t max, n; } ex_list_t;
static int ex_list_cb(exfat_vol_t *v, const ex_ent_t *e, void *arg) {
    ex_list_t *l = (ex_list_t *)arg;
    if (l->n >= l->max) return 1;
    vfs_dirent_t *d = &l->out[l->n++];
    ex_name_utf8(e, d->name, VFS_MAX_NAME);
    d->inode = e->first;
    d->type = (e->attr & EX_ATTR_DIR) ? VFS_DIRECTORY : VFS_FILE;
    d->size = e->len > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)e->len;
    return 0;
}

static int ex_readdir(vfs_filesystem_t *fs, const char *rel, vfs_dirent_t *ents, uint32_t max) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    ex_ent_t *e = (ex_ent_t *)kalloc(sizeof(ex_ent_t));
    if (!e) return -1;
    if (ex_lookup(v, rel ? rel : "", e) != 0 || !(e->attr & EX_ATTR_DIR)) { kfree(e); return -1; }
    ex_run_t dir = { e->first, e->flags, e->len };
    kfree(e);
    ex_list_t l = { ents, max, 0 };
    int wr = ex_walk_dir(v, &dir, ex_list_cb, &l);
    if (ex_debug) serial_printf("[exfat] readdir '%s' -> %d (rc %d)\n", rel ? rel : "", (int)l.n, wr);
    if (wr < 0) return -1;
    return (int)l.n;
}

static int ex_exists(vfs_filesystem_t *fs, const char *rel) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    if (rel && rel[0]=='.' && rel[1]=='!' && rel[2]==0) {
        serial_printf("[exfat] handles open=%d close=%d active=%d max=%d fail=%d\n",
                      ex_nopen, ex_nclose, ex_nopen-ex_nclose, ex_nmax, ex_nfail);
        return 1;
    }
    ex_ent_t *e = (ex_ent_t *)kalloc(sizeof(ex_ent_t));
    if (!e) return 0;
    int ok = ex_lookup(v, rel ? rel : "", e) == 0;
    kfree(e);
    return ok;
}

static int ex_create(vfs_filesystem_t *fs, const char *rel) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    return v->rw && ex_mark_dirty(v) == 0 ? ex_create_entry(v, rel, 0x20, NULL) : -1;
}
static int ex_mkdir(vfs_filesystem_t *fs, const char *rel) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    return v->rw && ex_mark_dirty(v) == 0 ? ex_create_entry(v, rel, EX_ATTR_DIR, NULL) : -1;
}
static int ex_unlink(vfs_filesystem_t *fs, const char *rel) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    if (!v->rw || ex_mark_dirty(v) != 0) return -1;
    return ex_unlink_entry(v, rel, false);
}

static int ex_rename(vfs_filesystem_t *fs, const char *orel, const char *nrel) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    if (!v->rw || ex_mark_dirty(v) != 0) return -EROFS_EX;
    ex_ent_t *o = (ex_ent_t *)kalloc(sizeof(ex_ent_t) * 3);
    if (!o) return -1;
    ex_ent_t *n = o + 1, *anc = o + 2;
    int rc = -ENOENT_EX;
    if (ex_lookup(v, orel, o) != 0 || o->is_root) goto out;
    bool odir = (o->attr & EX_ATTR_DIR) != 0;
    /* refuse moving a directory into itself / its own subtree: check every
     * ancestor of the new path against the old entry */
    if (odir) {
        static char pbuf[512];
        size_t L = strlen(nrel); if (L >= sizeof(pbuf)) { rc = -1; goto out; }
        memcpy(pbuf, nrel, L + 1);
        for (;;) {
            while (L > 0 && pbuf[L - 1] == '/') pbuf[--L] = 0;
            while (L > 0 && pbuf[L - 1] != '/') L--;
            if (L == 0) break;
            pbuf[--L] = 0;                              /* parent of current */
            if (ex_lookup(v, pbuf, anc) == 0 && !anc->is_root &&
                anc->parent.first == o->parent.first && anc->set_off == o->set_off) { rc = -EINVAL_EX; goto out; }
        }
    }
    if (ex_lookup(v, nrel, n) == 0) {
        bool same = !n->is_root && n->parent.first == o->parent.first && n->set_off == o->set_off;
        if (!same) {
            bool ndir = (n->attr & EX_ATTR_DIR) != 0;
            if (odir && !ndir) { rc = -ENOTDIR_EX; goto out; }
            if (!odir && ndir) { rc = -EISDIR_EX; goto out; }
            if (ex_unlink_entry(v, nrel, ndir) != 0) { rc = ndir ? -ENOTEMPTY_EX : -1; goto out; }
        } else {
            /* same entry (e.g. case change): drop the old set, re-create below */
        }
        if (same) {
            uint8_t set[19 * 32]; int k = 1 + o->nsec;
            if (ex_dir_io(v, &o->parent, o->set_off, set, k * 32, 0) != 0) { rc = -1; goto out; }
            for (int i = 0; i < k; i++) set[i * 32] &= 0x7F;
            if (ex_dir_io(v, &o->parent, o->set_off, set, k * 32, 1) != 0) { rc = -1; goto out; }
            rc = ex_create_entry(v, nrel, o->attr, o) == 0 ? 0 : -1;
            goto out;
        }
    }
    /* insert the new set first (a crash leaves a duplicate, never a loss) */
    if (ex_create_entry(v, nrel, o->attr, o) != 0) { rc = -1; goto out; }
    {
        uint8_t set[19 * 32]; int k = 1 + o->nsec;
        if (ex_dir_io(v, &o->parent, o->set_off, set, k * 32, 0) != 0) { rc = -1; goto out; }
        if (set[0] != 0x85) { rc = -1; goto out; }
        for (int i = 0; i < k; i++) set[i * 32] &= 0x7F;
        rc = ex_dir_io(v, &o->parent, o->set_off, set, k * 32, 1) == 0 ? 0 : -1;
    }
out:
    kfree(o);
    return rc;
}

static int ex_rmdir(vfs_filesystem_t *fs, const char *rel) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    if (!v->rw || ex_mark_dirty(v) != 0) return -EROFS_EX;
    ex_ent_t *e = (ex_ent_t *)kalloc(sizeof(ex_ent_t));
    if (!e) return -1;
    int rc = 0;
    if (ex_lookup(v, rel, e) != 0) rc = -ENOENT_EX;
    else if (e->is_root) rc = -EBUSY_EX;
    else if (!(e->attr & EX_ATTR_DIR)) rc = -ENOTDIR_EX;
    else { ex_run_t dr = { e->first, e->flags, e->len };
           if (ex_walk_dir(v, &dr, ex_any_cb, NULL) != 0) rc = -ENOTEMPTY_EX; }
    kfree(e);
    if (rc) return rc;
    return ex_unlink_entry(v, rel, true) == 0 ? 0 : -1;
}

static int ex_truncate(vfs_filesystem_t *fs, const char *rel, uint64_t len) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    if (!v->rw || ex_mark_dirty(v) != 0) return -EROFS_EX;
    ex_ent_t *e = (ex_ent_t *)kalloc(sizeof(ex_ent_t));
    if (!e) return -1;
    int rc = -ENOENT_EX;
    if (ex_lookup(v, rel, e) == 0 && !e->is_root) {
        if (e->attr & EX_ATTR_DIR) rc = -EISDIR_EX;
        else {
            uint32_t nc = ex_nclus(v, e->len), last = 0;
            if (nc) { ex_run_t r = { e->first, e->flags, e->len }; last = ex_run_clus(v, &r, nc - 1, NULL, NULL); }
            rc = ex_resize(v, e, len, &nc, &last) == 0 ? 0 : -ENOSPC_EX;
            ex_share(v, e, nc, last, NULL);
        }
    }
    kfree(e);
    return rc;
}

static int ex_ftruncate(vfs_filesystem_t *fs, void *handle, uint64_t len) {
    ex_handle_t *h = ex_h(handle);
    if (!h || !h->wr || !h->v->rw) return -1;
    if (ex_mark_dirty(h->v) != 0) return -1;
    int r = ex_resize(h->v, &h->e, len, &h->nclus, &h->last);
    ex_h_sync(h);
    ex_share(h->v, &h->e, h->nclus, h->last, h);
    return r == 0 ? 0 : -ENOSPC_EX;
}

static int ex_sync_op(vfs_filesystem_t *fs) {
    exfat_vol_t *v = (exfat_vol_t *)fs->fs_data;
    return exfat_sync(v);
}

static vfs_fs_ops_t exfat_ops = {
    .open = ex_open, .close = ex_close, .read = ex_read, .write = ex_write,
    .seek = ex_seek, .get_size = ex_get_size, .readdir = ex_readdir,
    .create = ex_create, .mkdir = ex_mkdir, .unlink = ex_unlink,
    .rename = ex_rename, .rmdir = ex_rmdir, .truncate = ex_truncate,
    .ftruncate = ex_ftruncate, .sync = ex_sync_op, .exists = ex_exists,
};

vfs_fs_ops_t *exfat_get_ops(void) { return &exfat_ops; }
const char *exfat_label(const exfat_vol_t *v) { return v ? v->label : ""; }

int exfat_vfs_register(const char *mount_path, exfat_vol_t *v) {
    int r = vfs_mount(mount_path, &exfat_ops, v);
    if (r == 0) {
        size_t l = strlen(mount_path);
        if (l >= sizeof(v->mpath)) l = sizeof(v->mpath) - 1;
        memcpy(v->mpath, mount_path, l); v->mpath[l] = 0;
        for (int i = 0; i < EX_MAX_VOLS; i++) if (!ex_vols[i]) { ex_vols[i] = v; break; }
    }
    return r;
}

exfat_vol_t *exfat_find_mounted(uint8_t dev, uint32_t part, const char **path) {
    for (int i = 0; i < EX_MAX_VOLS; i++)
        if (ex_vols[i] && ex_vols[i]->dev == dev && ex_vols[i]->part == part) {
            if (path) *path = ex_vols[i]->mpath;
            return ex_vols[i];
        }
    return NULL;
}
