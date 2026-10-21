#include "bootlog.h"
#include "serial.h"
#include "fat32.h"
#include "storage.h"
#include "mem.h"
#include "string.h"
#include "atomic.h"
#include "partscan.h"

extern uint64_t hpet_get_ticks(void);
extern int printf(const char *fmt, ...);
extern int snprintf(char *str, size_t size, const char *fmt, ...);

#define BL_HDR     512u                                  /* header sector    */
#define BL_CAP     (BOOTLOG_FILE_BYTES - BL_HDR)         /* log text capacity */
#define BL_KEEP    (256u * 1024u)                        /* kept when full    */
#define BL_DROP    (256u * 1024u)
#define BL_MAXCLUS 4096u
#define BL_CHUNK   64u                                   /* sectors per write */
#define BL_MAGIC   "inDevOS boot log"

static char bl_buf[BL_CAP];
static volatile uint32_t bl_len;
static volatile uint32_t bl_dirty = 0xFFFFFFFFu;   /* lowest changed offset */
static uint32_t bl_disk_len;                       /* text bytes on disk     */
static spinlock_t bl_lock;

static bool     bl_on;
static bool     bl_flushing;
static uint8_t  bl_dev;
static uint32_t bl_spc;                            /* sectors per cluster */
static uint32_t bl_nclus;
static uint32_t bl_txt_lba[BL_MAXCLUS];           /* first LBA of each cluster */
static uint32_t bl_old_lba[BL_MAXCLUS];
static bool     bl_have_old;
static uint32_t bl_ent_lba, bl_ent_off, bl_first_clus;
static uint8_t  bl_ent_bytes[32];                  /* dir/stream entry as found */
static const char *bl_where = "";
static uint32_t bl_boot_no;
static uint64_t bl_last_flush;
static uint32_t bl_flushes, bl_errors;
static volatile bool bl_panic;                     /* exception context: no locks */

static inline uint64_t bl_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void bl_irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

void bootlog_putc(char c) {
    uint64_t f = bl_irq_save();
    bool lk = !bl_panic;
    if (lk) spinlock_lock(&bl_lock);
    if (bl_len >= BL_CAP) {
        /* Full: keep the first 256 KB (early boot) and the newest part. */
        memmove(bl_buf + BL_KEEP, bl_buf + BL_KEEP + BL_DROP, BL_CAP - BL_KEEP - BL_DROP);
        bl_len = BL_CAP - BL_DROP;
        static const char mark[] = "\n[bootlog] ---- older lines dropped (buffer full) ----\n";
        memcpy(bl_buf + BL_KEEP, mark, sizeof(mark) - 1);
        if (bl_dirty > BL_KEEP) bl_dirty = BL_KEEP;
    }
    if (bl_len < bl_dirty) bl_dirty = bl_len;
    bl_buf[bl_len++] = c;
    if (lk) spinlock_unlock(&bl_lock);
    bl_irq_restore(f);
}

bool bootlog_attached(void) { return bl_on; }

/* Read/write file sectors [s0, s0+n) of the file whose clusters start at
 * @lbas[], batching runs that are contiguous on disk. */
static int bl_io(const uint32_t *lbas, uint32_t s0, uint32_t n, uint8_t *buf, bool wr) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t s = s0 + i;
        uint32_t ci = s / bl_spc;
        if (ci >= bl_nclus) return -1;
        uint32_t lba = lbas[ci] + s % bl_spc;
        uint32_t run = 1;
        while (i + run < n) {
            uint32_t s2 = s + run, c2 = s2 / bl_spc;
            if (c2 >= bl_nclus || lbas[c2] + s2 % bl_spc != lba + run) break;
            run++;
        }
        int rc = wr ? storage_write_sectors(bl_dev, lba, run, buf + i * 512u)
                    : storage_read_sectors(bl_dev, lba, run, buf + i * 512u);
        if (rc != 0) return -1;
        i += run;
    }
    return 0;
}
static int bl_write_file_sectors(uint32_t s0, uint32_t n, const uint8_t *src) {
    return bl_io(bl_txt_lba, s0, n, (uint8_t *)src, true);
}

/* The directory entry must be byte-identical to what we found at attach
 * (same file, first cluster, sizes); if someone deleted/replaced the file,
 * stop writing (its clusters may have been reused). */
static bool bl_entry_still_ours(void) {
    static uint8_t sec[512];
    if (storage_read_sectors(bl_dev, bl_ent_lba, 1, sec) != 0) return false;
    return memcmp(sec + bl_ent_off, bl_ent_bytes, 32) == 0;
}

void bootlog_flush(void) {
    if (!bl_on || bl_flushing) return;
    bl_flushing = true;
    static uint8_t chunk[BL_CHUNK * 512u];
    uint64_t f = bl_irq_save();
    spinlock_lock(&bl_lock);
    uint32_t len = bl_len, from = bl_dirty;
    bl_dirty = 0xFFFFFFFFu;
    spinlock_unlock(&bl_lock);
    bl_irq_restore(f);
    uint32_t end = len > bl_disk_len ? len : bl_disk_len;   /* blank a shrunk tail */
    if (from >= end) { bl_flushing = false; return; }
    if (!bl_entry_still_ours()) {
        bl_on = false;
        bl_flushing = false;
        serial_printf("[bootlog] %s changed on disk - stopped writing it\n", bl_where);
        return;
    }
    /* text offset -> file offset (+ header sector) */
    uint32_t s_first = (BL_HDR + from) / 512u;
    uint32_t s_end = (BL_HDR + end + 511u) / 512u;
    for (uint32_t s = s_first; s < s_end; s += BL_CHUNK) {
        uint32_t n = s_end - s < BL_CHUNK ? s_end - s : BL_CHUNK;
        f = bl_irq_save();
        spinlock_lock(&bl_lock);
        uint32_t cur = bl_len;
        for (uint32_t k = 0; k < n * 512u; k++) {
            uint32_t off = s * 512u + k - BL_HDR;          /* s >= 1 always */
            chunk[k] = off < cur ? (uint8_t)bl_buf[off] : (uint8_t)' ';
        }
        spinlock_unlock(&bl_lock);
        bl_irq_restore(f);
        if (bl_write_file_sectors(s, n, chunk) != 0) {
            bl_errors++;
            /* retry these bytes next time */
            f = bl_irq_save(); spinlock_lock(&bl_lock);
            uint32_t o = s * 512u - BL_HDR;
            if (o < bl_dirty) bl_dirty = o;
            spinlock_unlock(&bl_lock); bl_irq_restore(f);
            bl_flushing = false;
            return;
        }
    }
    storage_flush(bl_dev);
    bl_disk_len = len;
    bl_flushes++;
    bl_last_flush = hpet_get_ticks();
    bl_flushing = false;
}

/* ---- exception context (best effort, no locks, never returns errors) ---- */
static inline void bl_outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t bl_inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

/* Text straight to COM1 and the RAM log, bypassing serial_out_lock/bl_lock
 * (the faulting code may hold either). */
void bootlog_panic_puts(const char *s) {
    bl_panic = true;
    for (; *s; s++) {
        for (int i = 0; i < 100000 && !(bl_inb(0x3F8 + 5) & 0x20); i++) { }
        bl_outb(0x3F8, (uint8_t)*s);
        bootlog_putc(*s);
    }
}

/* Write the unwritten part of the log to the file from an exception
 * handler: no spinlocks, its own buffer, runs at most once at a time. */
void bootlog_flush_emergency(void) {
    static volatile int busy;
    static uint8_t ec[8 * 512];
    if (!bl_on || busy) return;
    busy = 1;
    bl_panic = true;
    uint32_t len = bl_len, from = bl_dirty;
    uint32_t end = len > bl_disk_len ? len : bl_disk_len;
    if (from == 0xFFFFFFFFu || from >= end || !bl_entry_still_ours()) { busy = 0; return; }
    bl_dirty = 0xFFFFFFFFu;
    uint32_t s_first = (BL_HDR + from) / 512u, s_end = (BL_HDR + end + 511u) / 512u;
    for (uint32_t s = s_first; s < s_end; s += 8) {
        uint32_t n = s_end - s < 8 ? s_end - s : 8;
        for (uint32_t k = 0; k < n * 512u; k++) {
            uint32_t off = s * 512u + k - BL_HDR;
            ec[k] = off < len ? (uint8_t)bl_buf[off] : (uint8_t)' ';
        }
        if (bl_write_file_sectors(s, n, ec) != 0) { bl_errors++; break; }
    }
    storage_flush(bl_dev);
    bl_disk_len = len;
    busy = 0;
}

void bootlog_tick(void) {
    if (!bl_on || bl_dirty == 0xFFFFFFFFu) return;
    uint64_t now = hpet_get_ticks();
    /* every second for the first two minutes of uptime, then every 5 s */
    uint64_t period = now < 120000 ? 1000 : 5000;
    if (now - bl_last_flush >= period) bootlog_flush();
}

void bootlog_stage(const char *name) {
    serial_printf("[stage] %s\n", name);
    bootlog_flush();
}

/* Find an 8.3 name in the root directory: cluster, size, entry location. */
static int bl_find_root(fat32_fs_t *fs, const char *n83, uint32_t *clus, uint32_t *size,
                        uint32_t *ent_lba, uint32_t *ent_off) {
    uint8_t *buf = (uint8_t *)kalloc(fs->bytes_per_cluster);
    if (!buf) return -1;
    uint32_t c = fs->root_dir_cluster;
    for (int guard = 0; c >= 2 && c < 0x0FFFFFF8u && guard < 4096; guard++) {
        if (fat32_read_cluster(fs, c, buf) != 0) break;
        uint32_t per = fs->bytes_per_cluster / 32u;
        for (uint32_t i = 0; i < per; i++) {
            fat32_dir_entry_t *e = (fat32_dir_entry_t *)(buf + i * 32u);
            if ((uint8_t)e->name[0] == 0x00) { kfree(buf); return -1; }
            if ((uint8_t)e->name[0] == 0xE5 || e->attr == FAT32_ATTR_LFN) continue;
            if (e->attr & (FAT32_ATTR_DIRECTORY | FAT32_ATTR_VOLUME_ID)) continue;
            if (memcmp(e->name, n83, 11) != 0) continue;
            *clus = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
            *size = e->file_size;
            *ent_lba = fat32_cluster_to_sector(fs, c) + (i * 32u) / 512u;
            *ent_off = (i * 32u) % 512u;
            kfree(buf);
            return 0;
        }
        c = fat32_get_next_cluster(fs, c);
    }
    kfree(buf);
    return -1;
}

static int bl_chain(fat32_fs_t *fs, uint32_t first, uint32_t *lbas, uint32_t need) {
    uint32_t c = first, n = 0;
    while (n < need && c >= 2 && c < 0x0FFFFFF8u && n < BL_MAXCLUS) {
        lbas[n++] = fat32_cluster_to_sector(fs, c);
        c = fat32_get_next_cluster(fs, c);
    }
    return (int)n;
}

/* First sector must be our header or blank (spaces) - never overwrite a
 * file that holds something else. */
static bool bl_sector_ok(const uint8_t *p) {
    if (memcmp(p, BL_MAGIC, sizeof(BL_MAGIC) - 1) == 0) return true;
    for (int i = 0; i < 512; i++) if (p[i] != ' ' && p[i] != '\r' && p[i] != '\n') return false;
    return true;
}

/* Common part once TXT (and optionally OLD) cluster tables are filled in. */
static void bl_start(const char *where, const char *devname) {
    uint8_t *tmp = (uint8_t *)kalloc(BL_CHUNK * 512u);
    if (!tmp) return;
    if (bl_io(bl_txt_lba, 0, 1, tmp, false) != 0 || !bl_sector_ok(tmp)) {
        serial_printf("[bootlog] %s does not start with the boot-log header or spaces - not touching it\n", where);
        kfree(tmp);
        return;
    }
    uint32_t prev_no = 0;
    bool prev_ok = false;
    if (memcmp(tmp, BL_MAGIC, sizeof(BL_MAGIC) - 1) == 0) {
        const char *p = (const char *)tmp;
        for (int i = 0; i < 480; i++)
            if (p[i] == '#') {
                for (int k = i + 1; k < 500 && p[k] >= '0' && p[k] <= '9'; k++)
                    prev_no = prev_no * 10 + (uint32_t)(p[k] - '0');
                break;
            }
        prev_ok = prev_no > 0;
    }
    bl_boot_no = prev_no + 1;

    static uint8_t chk[512];
    if (prev_ok && bl_have_old && bl_io(bl_old_lba, 0, 1, chk, false) == 0 && bl_sector_ok(chk)) {
        uint32_t total = BOOTLOG_FILE_BYTES / 512u, done = 0;
        int rc = 0;
        for (uint32_t s = 0; s < total && rc == 0; s += BL_CHUNK) {
            rc = bl_io(bl_txt_lba, s, BL_CHUNK, tmp, false);
            if (rc == 0) rc = bl_io(bl_old_lba, s, BL_CHUNK, tmp, true);
            if (rc == 0) done += BL_CHUNK;
        }
        serial_printf("[bootlog] previous log (boot #%u) saved to the .OLD file (%u sectors, rc=%d)\n",
                      prev_no, done, rc);
    }

    memset(tmp, ' ', 512);
    char h[400];
    int hn = snprintf(h, sizeof(h),
        BL_MAGIC " - boot #%u\n"
        "Newest boot first: this file. The boot before it: the .OLD file.\n"
        "Text is written straight to disk while booting; the last line is\n"
        "how far the kernel got. Trailing spaces are unused space.\n"
        "--------------------------------------------------------------------\n",
        bl_boot_no);
    if (hn > 0) memcpy(tmp, h, (size_t)(hn < 510 ? hn : 510));
    tmp[511] = '\n';
    bl_where = where;
    bl_on = true;
    if (bl_write_file_sectors(0, 1, tmp) != 0) {
        bl_on = false;
        kfree(tmp);
        serial_puts("[bootlog] header write failed - log stays in RAM only\n");
        return;
    }
    kfree(tmp);
    serial_printf("[bootlog] writing boot #%u log to %s on storage device %u (%s), %u clusters of %u sectors\n",
                  bl_boot_no, where, bl_dev, devname ? devname : "?", bl_nclus, bl_spc);
    bl_disk_len = BL_CAP;
    uint64_t fl = bl_irq_save();
    spinlock_lock(&bl_lock);
    bl_dirty = 0;
    spinlock_unlock(&bl_lock);
    bl_irq_restore(fl);
    bootlog_flush();
}

/* ---- FAT32 (the INDEVOS root volume when the stick is os_install.iso) ---- */
void bootlog_attach(void) {
    if (bl_on) return;
    fat32_fs_t *fs = &fat32_filesystem;
    if (!fs->is_mounted || !fs->storage_dev || fs->bytes_per_cluster == 0) {
        serial_puts("[bootlog] no FAT boot volume mounted - log stays in RAM only\n");
        return;
    }
    uint32_t tc, ts, oc, os, olba, ooff;
    if (bl_find_root(fs, "BOOTLOG TXT", &tc, &ts, &bl_ent_lba, &bl_ent_off) != 0 ||
        ts < BOOTLOG_FILE_BYTES || tc < 2) {
        serial_puts("[bootlog] /BOOTLOG.TXT (1 MiB, preallocated) not on the boot volume - log stays in RAM only\n");
        return;
    }
    bl_dev = fs->storage_dev->device_id;
    bl_spc = fs->sectors_per_cluster;
    uint32_t need = BOOTLOG_FILE_BYTES / fs->bytes_per_cluster;
    if (need == 0 || need > BL_MAXCLUS ||
        bl_chain(fs, tc, bl_txt_lba, need) != (int)need) {
        serial_printf("[bootlog] /BOOTLOG.TXT cluster chain unusable (need %u clusters)\n", need);
        return;
    }
    bl_nclus = need;
    bl_first_clus = tc;
    {
        static uint8_t sec[512];
        if (storage_read_sectors(bl_dev, bl_ent_lba, 1, sec) != 0) return;
        memcpy(bl_ent_bytes, sec + bl_ent_off, 32);
    }
    bl_have_old = bl_find_root(fs, "BOOTLOG OLD", &oc, &os, &olba, &ooff) == 0 &&
                  os >= BOOTLOG_FILE_BYTES && bl_chain(fs, oc, bl_old_lba, need) == (int)need;
    bl_start("/BOOTLOG.TXT", fs->storage_dev->name);
}

/* ---- exFAT (Ventoy data partition), read-only lookup ---------------------- */
extern storage_device_t storage_devices[];
extern uint8_t storage_device_count;

typedef struct {
    uint8_t  dev;
    uint32_t part, fat_off, heap_off, clus_count, root, spc;
    uint32_t fat_cached;
    uint8_t  fat_sec[512];
} bl_exfat_t;

static uint32_t bx_lba(bl_exfat_t *x, uint32_t c) { return x->part + x->heap_off + (c - 2) * x->spc; }

static uint32_t bx_next(bl_exfat_t *x, uint32_t c) {
    uint32_t sec = x->part + x->fat_off + (c * 4u) / 512u;
    if (x->fat_cached != sec) {
        if (storage_read_sectors(x->dev, sec, 1, x->fat_sec) != 0) return 0xFFFFFFFFu;
        x->fat_cached = sec;
    }
    return *(uint32_t *)(x->fat_sec + (c * 4u) % 512u);
}

typedef struct {
    bool     found;
    uint32_t first, ent_lba, ent_off;
    uint64_t len, vdl;
    uint8_t  flags;
    uint8_t  ent[32];
} bx_file_t;

static bool bx_name_eq(const uint16_t *u, int n, const char *want) {
    int i = 0;
    for (; i < n && want[i]; i++) {
        uint16_t a = u[i];
        char b = want[i];
        if (a >= 'a' && a <= 'z') a = (uint16_t)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != (uint16_t)(uint8_t)b) return false;
    }
    return i == n && want[i] == '\0';
}

/* Walk the root directory entry sets: 0x85 File, 0xC0 Stream Extension,
 * 0xC1 File Name.  Fills @txt / @old for the two names. */
static void bx_scan_root(bl_exfat_t *x, const char *ntxt, const char *nold,
                         bx_file_t *txt, bx_file_t *old) {
    static uint8_t sec[512];
    int remaining = 0, namelen = 0, got = 0;
    uint16_t name[64];
    bx_file_t cur;
    memset(&cur, 0, sizeof(cur));
    uint32_t c = x->root;
    for (int guard = 0; c >= 2 && c < x->clus_count + 2 && guard < 256; guard++) {
        for (uint32_t s = 0; s < x->spc; s++) {
            uint32_t lba = bx_lba(x, c) + s;
            if (storage_read_sectors(x->dev, lba, 1, sec) != 0) return;
            for (uint32_t o = 0; o < 512; o += 32) {
                uint8_t t = sec[o];
                if (t == 0x00) return;                       /* end of directory */
                if (t == 0x85) {
                    remaining = sec[o + 1];
                    namelen = 0; got = 0;
                    memset(&cur, 0, sizeof(cur));
                    continue;
                }
                if (remaining <= 0) continue;
                remaining--;
                if (t == 0xC0) {
                    cur.flags = sec[o + 1];
                    namelen = sec[o + 3];
                    cur.vdl = *(uint64_t *)(sec + o + 8);
                    cur.first = *(uint32_t *)(sec + o + 20);
                    cur.len = *(uint64_t *)(sec + o + 24);
                    cur.ent_lba = lba; cur.ent_off = o;
                    memcpy(cur.ent, sec + o, 32);
                } else if (t == 0xC1) {
                    for (int k = 0; k < 15 && got < namelen && got < 64; k++)
                        name[got++] = *(uint16_t *)(sec + o + 2 + 2 * k);
                }
                if (remaining == 0 && namelen > 0 && got == namelen) {
                    cur.found = true;
                    if (bx_name_eq(name, namelen, ntxt)) *txt = cur;
                    else if (bx_name_eq(name, namelen, nold)) *old = cur;
                }
            }
        }
        c = bx_next(x, c);
    }
}

static int bx_clusters(bl_exfat_t *x, const bx_file_t *f, uint32_t *lbas, uint32_t need) {
    uint32_t c = f->first, n = 0;
    while (n < need) {
        if (c < 2 || c >= x->clus_count + 2) return -1;
        lbas[n++] = bx_lba(x, c);
        c = (f->flags & 0x02) ? c + 1 : bx_next(x, c);      /* NoFatChain: contiguous */
    }
    return 0;
}

static const char BX_TXT[] = "INDEVOS_BOOTLOG.TXT";
static const char BX_OLD[] = "INDEVOS_BOOTLOG.OLD";

static bool bx_try_volume(uint8_t dev, uint32_t part, const char *devname) {
    static uint8_t bs[512];
    if (storage_read_sectors(dev, part, 1, bs) != 0) return false;
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0) return false;
    static bl_exfat_t x;
    memset(&x, 0, sizeof(x));
    x.dev = dev; x.part = part;
    x.fat_off = *(uint32_t *)(bs + 80);
    x.heap_off = *(uint32_t *)(bs + 88);
    x.clus_count = *(uint32_t *)(bs + 92);
    x.root = *(uint32_t *)(bs + 96);
    x.fat_cached = 0xFFFFFFFFu;
    uint8_t bps_shift = bs[108], spc_shift = bs[109];
    serial_printf("[bootlog] exFAT volume on device %u (%s) at LBA %u: %u-byte sectors, %u sectors/cluster\n",
                  dev, devname, part, 1u << bps_shift, 1u << spc_shift);
    if (bps_shift != 9 || spc_shift > 16) {
        serial_puts("[bootlog] exFAT geometry unsupported (need 512-byte sectors)\n");
        return false;
    }
    x.spc = 1u << spc_shift;
    bx_file_t txt, old;
    memset(&txt, 0, sizeof(txt)); memset(&old, 0, sizeof(old));
    bx_scan_root(&x, BX_TXT, BX_OLD, &txt, &old);
    if (!txt.found) {
        serial_printf("[bootlog] no %s in the root of that volume\n", BX_TXT);
        return false;
    }
    if (txt.len < BOOTLOG_FILE_BYTES || txt.vdl < txt.len || !(txt.flags & 0x01)) {
        serial_printf("[bootlog] %s unusable: size %u, valid data length %u, flags 0x%x "
                      "(need >= 1 MiB fully written; run make_ventoy_bootlog.ps1)\n",
                      BX_TXT, (uint32_t)txt.len, (uint32_t)txt.vdl, txt.flags);
        return false;
    }
    uint32_t cbytes = x.spc * 512u;
    uint32_t need = (BOOTLOG_FILE_BYTES + cbytes - 1) / cbytes;
    if (need > BL_MAXCLUS || bx_clusters(&x, &txt, bl_txt_lba, need) != 0) {
        serial_printf("[bootlog] %s cluster chain unusable\n", BX_TXT);
        return false;
    }
    bl_dev = dev; bl_spc = x.spc; bl_nclus = need;
    bl_first_clus = txt.first;
    bl_ent_lba = txt.ent_lba; bl_ent_off = txt.ent_off;
    memcpy(bl_ent_bytes, txt.ent, 32);
    bl_have_old = old.found && old.len >= BOOTLOG_FILE_BYTES && old.vdl >= old.len &&
                  bx_clusters(&x, &old, bl_old_lba, need) == 0;
    if (!bl_have_old) serial_printf("[bootlog] %s missing or unusable - no rotation\n", BX_OLD);
    bl_start("INDEVOS_BOOTLOG.TXT (exFAT)", devname);
    return bl_on;
}

/* Right after USB storage enumeration: look on every USB disk (MBR or GPT
 * partitions, or a partitionless volume) for an exFAT volume holding
 * INDEVOS_BOOTLOG.TXT, e.g. a Ventoy stick's data partition. */
void bootlog_attach_usb(void) {
    if (bl_on) return;
    static uint8_t s0[512], s1[512];
    static uint8_t ents[512];
    for (uint8_t d = 0; d < storage_device_count && !bl_on; d++) {
        storage_device_t *dev = &storage_devices[d];
        if (!dev->pci_dev || dev->pci_dev->class_code != 0x0C || dev->pci_dev->subclass_code != 0x03)
            continue;
        uint8_t id = dev->device_id;
        uint32_t starts[17];
        int ns = partscan_starts(id, starts, 17);
        for (int i = 0; i < ns && !bl_on; i++)
            bx_try_volume(id, starts[i], dev->name);
    }
    if (!bl_on) serial_puts("[bootlog] no exFAT INDEVOS_BOOTLOG.TXT on USB storage yet\n");
}

void bootlog_status(void) {
    printf("bootlog: writing %s, boot #%u, %u bytes in RAM, %u on disk, %u flushes, %u errors\n",
           bl_on ? bl_where : "RAM only", bl_boot_no, bl_len, bl_disk_len,
           bl_flushes, bl_errors);
}
