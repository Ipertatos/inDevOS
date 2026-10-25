#include "fat32.h"
#include "kprof.h"
#include "utils.h"
#include "mem.h"
#include "debug.h"
#include "string.h"
#include "serial.h"
#include "atomic.h"
#include "hpet.h"
#include "ktime.h"   /* ktime_now_us() for the read diagnostic */
#include "debug_config.h"
#include "../include/indevos_errno.h"

/*
 * FAT32 subsystem lock.
 *
 * Previous approach disabled interrupts globally which caused latency spikes
 * during cluster-chain walks and storage I/O.  Now uses a spinlock instead
 * so IRQs remain enabled during filesystem operations.
 */
static spinlock_t fat32_lock = {0};
#define FS_LOCK()   do { klock(&fat32_lock); } while(0)
#define FS_UNLOCK() do { kunlock(&fat32_lock); } while(0)

/*
 * Upper bound for cluster-chain walks. A corrupted FAT containing a cycle
 * would otherwise spin the kernel forever; no legitimate FAT32 chain can
 * have more clusters than the FAT has entries (max ~2^28), and for this OS
 * 2^20 clusters (4 GiB at 4 KiB/cluster) is a generous practical cap.
 */
#define FAT32_MAX_CHAIN_CLUSTERS (1u << 20)

// Forward declarations for helper functions used before their definitions
static uint32_t fat32_allocate_cluster(fat32_fs_t *fs);
static int fat32_flush_fat(fat32_fs_t *fs);
static void fat32_mark_fat_dirty(fat32_fs_t *fs, uint32_t cluster);
static void fat32_free_cluster_chain(fat32_fs_t *fs, uint32_t start_cluster);

fat32_fs_t fat32_filesystem;
fat32_file_t open_files[FAT32_MAX_OPEN_FILES];

/* FAT dirty-sector range tracking: fat32_flush_fat() writes only the FAT
 * sectors actually modified since the last flush, instead of rewriting the
 * entire FAT (potentially megabytes) on every cluster allocation. */
static uint32_t fat_dirty_lo = 0xFFFFFFFF;
static uint32_t fat_dirty_hi = 0;
/* Next-fit cursor for free-cluster search: avoids re-scanning from cluster 2
 * on every allocation (O(n^2) when filling a file cluster-by-cluster). */
static uint32_t fat_alloc_cursor = 2;
/* Write-back FAT: fat32_close() flushes the dirty FAT sectors once per file
 * instead of every fat32_write() call flushing them. While fat_defer_flush is
 * set (installer bulk copy) even that waits for fat32_sync()/unmount. */
static bool fat_defer_flush = false;
/* Set once this mount changed the FAT; only then is FSInfo rewritten, so a
 * read-only use of a disk (the installer's live disk) never writes to it. */
static bool fat_meta_changed = false;
/* One-shot hint from fat32_create_file(): where the entry it just placed
 * lives, so the open that follows (vfs_open O_CREAT) does not walk the path
 * and rescan the parent directory. Verified against the on-disk slot before
 * use and dropped by rename/delete/mount/unmount. */
static struct {
    bool valid;
    char path[MAX_FILENAME_LEN];
    uint32_t parent;
    uint32_t cluster;
    uint32_t index;
    char name[11];
} fat_create_hint;
static uint32_t fat_last_place_cluster, fat_last_place_index;
static int fat32_sync_locked(void);
static char fat_last_place_name[11];

/*
 * The FAT is not resident. A 32 GB FAT32 volume is several megabytes of FAT
 * (and a larger disk is worse); loading it in fat32_mount made mount and the
 * first directory listing scale with the whole disk. Slots are read on demand
 * and written back only when an entry changes.
 */
#define FAT32_FAT_CACHE_SLOTS 32
typedef struct {
    uint32_t sector; /* FAT-relative, 0xFFFFFFFF = empty */
    uint8_t dirty;
    uint8_t data[512];
} fat32_fat_slot_t;
static fat32_fat_slot_t fat_slots[FAT32_FAT_CACHE_SLOTS];
static uint32_t fat_slot_hand = 0;
static bool fat_cache_live = false;

static void fat32_fat_cache_reset(void) {
    for (int i = 0; i < FAT32_FAT_CACHE_SLOTS; i++) {
        fat_slots[i].sector = 0xFFFFFFFFu;
        fat_slots[i].dirty = 0;
    }
    fat_slot_hand = 0;
    fat_cache_live = false;
}

static int fat32_fat_slot_write(fat32_fs_t *fs, fat32_fat_slot_t *slot) {
    if (!slot->dirty || slot->sector == 0xFFFFFFFFu)
        return 0;
    if (slot->sector >= fs->boot_sector.fat_size_32)
        return -1;
    uint8_t id = fs->storage_dev->device_id;
    if (storage_write_sectors(id, fs->fat_start_sector + slot->sector, 1, slot->data) != 0)
        return -1;
    if (fs->boot_sector.num_fats > 1 && (fs->boot_sector.ext_flags & 0x80) == 0) {
        uint8_t copies = fs->boot_sector.num_fats;
        if (copies > 4) copies = 4;
        for (uint8_t fat = 1; fat < copies; fat++) {
            uint32_t base = fs->fat_start_sector + (uint32_t)fat * fs->boot_sector.fat_size_32;
            if (storage_write_sectors(id, base + slot->sector, 1, slot->data) != 0)
                return -1;
        }
    }
    slot->dirty = 0;
    return 0;
}

/* Write `count` consecutive FAT sectors starting at FAT-relative `sector`
 * to every mirrored FAT copy. */
static int fat32_fat_write_run(fat32_fs_t *fs, uint32_t sector, uint32_t count,
                               const void *data) {
    if (count == 0 || sector >= fs->boot_sector.fat_size_32 ||
        count > fs->boot_sector.fat_size_32 - sector)
        return -1;
    uint8_t id = fs->storage_dev->device_id;
    uint8_t copies = 1;
    if (fs->boot_sector.num_fats > 1 && (fs->boot_sector.ext_flags & 0x80) == 0) {
        copies = fs->boot_sector.num_fats;
        if (copies > 4) copies = 4;
    }
    for (uint8_t fat = 0; fat < copies; fat++) {
        uint32_t base = fs->fat_start_sector + (uint32_t)fat * fs->boot_sector.fat_size_32;
        if (storage_write_sectors(id, base + sector, count, data) != 0)
            return -1;
    }
    return 0;
}

static fat32_fat_slot_t *fat32_fat_slot(fat32_fs_t *fs, uint32_t sector) {
    for (int i = 0; i < FAT32_FAT_CACHE_SLOTS; i++) {
        if (fat_slots[i].sector == sector)
            return &fat_slots[i];
    }
    /* Pick a victim round-robin. A dirty slot whose write-back fails keeps
     * its data (and stays dirty) and is skipped: failing the lookup instead
     * made fat32_get_next_cluster() report a false end of chain for an
     * unrelated sector, so seeks deep into a file stopped early and reads
     * returned the wrong clusters with a full byte count (doom1.wad's
     * directory on the boot CD, where writes always fail). */
    fat32_fat_slot_t *slot = NULL;
    for (int tries = 0; tries < FAT32_FAT_CACHE_SLOTS; tries++) {
        fat32_fat_slot_t *cand = &fat_slots[fat_slot_hand];
        fat_slot_hand = (fat_slot_hand + 1) % FAT32_FAT_CACHE_SLOTS;
        if (cand->dirty && fat32_fat_slot_write(fs, cand) != 0)
            continue;
        slot = cand;
        break;
    }
    if (!slot)
        return NULL;
    if (storage_read_sectors(fs->storage_dev->device_id,
                             fs->fat_start_sector + sector, 1, slot->data) != 0) {
        slot->sector = 0xFFFFFFFFu;
        slot->dirty = 0;
        return NULL;
    }
    slot->sector = sector;
    slot->dirty = 0;
    return slot;
}

static int fat32_fat_load(fat32_fs_t *fs, uint32_t cluster, uint32_t *raw) {
    if (!fat_cache_live || !fs || !fs->storage_dev || cluster < 2)
        return -1;
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    if (bps == 0) bps = 512;
    uint64_t max_entries = ((uint64_t)fs->boot_sector.fat_size_32 * bps) / 4;
    if ((uint64_t)cluster >= max_entries)
        return -1;
    uint32_t sector = (uint32_t)(((uint64_t)cluster * 4) / bps);
    uint32_t off = (uint32_t)(((uint64_t)cluster * 4) % bps);
    fat32_fat_slot_t *slot = fat32_fat_slot(fs, sector);
    if (!slot)
        return -1;
    memcpy(raw, slot->data + off, 4);
    return 0;
}

static int fat32_fat_store(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (!fat_cache_live || !fs || !fs->storage_dev || cluster < 2)
        return -1;
    /* Read-only medium (boot CD): never dirty the cached FAT. The change
     * could not be written back, and a dirty slot that can never be cleaned
     * used to poison later FAT lookups. */
    if (fs->storage_dev->optical)
        return -1;
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    if (bps == 0) bps = 512;
    uint64_t max_entries = ((uint64_t)fs->boot_sector.fat_size_32 * bps) / 4;
    if ((uint64_t)cluster >= max_entries)
        return -1;
    uint32_t sector = (uint32_t)(((uint64_t)cluster * 4) / bps);
    uint32_t off = (uint32_t)(((uint64_t)cluster * 4) % bps);
    fat32_fat_slot_t *slot = fat32_fat_slot(fs, sector);
    if (!slot)
        return -1;
    memcpy(slot->data + off, &value, 4);
    slot->dirty = 1;
    fat_meta_changed = true;
    fat32_mark_fat_dirty(fs, cluster);
    return 0;
}

static void fat32_init_file_table() {
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        open_files[i].in_use = false;
    }
}

uint32_t fat32_cluster_to_sector(fat32_fs_t *fs, uint32_t cluster) {
    if (cluster < 2) {
        return 0; // Invalid cluster
    }
    return fs->data_start_sector + (cluster - 2) * fs->sectors_per_cluster;
}

uint32_t fat32_get_next_cluster(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t raw = 0;
    if (fat32_fat_load(fs, cluster, &raw) != 0) {
        return FAT32_CLUSTER_END;
    }

    uint32_t next_cluster = raw & 0x0FFFFFFF;
    
    if (next_cluster >= 0x0FFFFFF8) {
        return FAT32_CLUSTER_END;
    }
    if (next_cluster == 0x0FFFFFF7) {
        return FAT32_CLUSTER_BAD;
    }
    if (next_cluster == 0) {
        return FAT32_CLUSTER_FREE;
    }
    
    return next_cluster;
}

int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, void *buffer) {
    if (!fs) {
        printf("fat32_read_cluster: fs is NULL!\n");
        return -1;
    }
    if (!fs->storage_dev) {
        printf("fat32_read_cluster: storage_dev is NULL!\n");
        return -1;
    }
    if (!buffer) {
        printf("fat32_read_cluster: buffer is NULL!\n");
        return -1;
    }
    uint32_t start_sector = fat32_cluster_to_sector(fs, cluster);
    if (start_sector == 0) {
        printf("fat32: Invalid cluster %d - cluster_to_sector returned 0\n", cluster);
        return -1;
    }
    
    // Debug: printf("fat32: Reading cluster %d at sector %d\n", cluster, start_sector);
    
    int result = storage_read_sectors(fs->storage_dev->device_id, start_sector, 
                                    fs->sectors_per_cluster, buffer);
    if (result != 0) {
        printf("fat32: Storage read failed for sector %d (result: %d)\n", start_sector, result);
    }
    
    return result;
}

int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const void *buffer) {
    if (!fs) {
        printf("fat32_write_cluster: fs is NULL!\n");
        return -1;
    }
    if (!fs->storage_dev) {
        printf("fat32_write_cluster: storage_dev is NULL!\n");
        return -1;
    }
    if (!buffer) {
        printf("fat32_write_cluster: buffer is NULL!\n");
        return -1;
    }
    uint32_t start_sector = fat32_cluster_to_sector(fs, cluster);
    if (start_sector == 0) {
        return -1;
    }
    
    return storage_write_sectors(fs->storage_dev->device_id, start_sector, 
                               fs->sectors_per_cluster, buffer);
}

void fat32_parse_83_filename(const char *long_name, char *short_name) {
    memset(short_name, ' ', 11);
    
    int name_len = 0;
    int ext_len = 0;
    bool in_extension = false;
    
    /* Walk the whole name: stopping once 8 base characters were copied
     * dropped the extension of 8.3 names with an 8-character base
     * (NEOFETCH.EXE became "NEOFETCH   " and could never be found again). */
    for (int i = 0; long_name[i]; i++) {
        char c = long_name[i];
        
        if (c == '.') {
            in_extension = true;
            continue;
        }
        
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A'; // Convert to uppercase
        }
        
        if (in_extension && ext_len < 3) {
            short_name[8 + ext_len++] = c;
        } else if (!in_extension && name_len < 8) {
            short_name[name_len++] = c;
        }
    }
}

void fat32_format_83_filename(const char *short_name, char *formatted_name) {
    int pos = 0;
    
    // Copy name part
    for (int i = 0; i < 8 && short_name[i] != ' '; i++) {
        formatted_name[pos++] = short_name[i];
    }
    
    // Add extension if present
    if (short_name[8] != ' ') {
        formatted_name[pos++] = '.';
        for (int i = 8; i < 11 && short_name[i] != ' '; i++) {
            formatted_name[pos++] = short_name[i];
        }
    }
    
    formatted_name[pos] = '\0';
}

// New helper: format an 8.3 short name applying NT-reserved lowercase flags
static void fat32_format_83_filename_display(const char *short_name,
                                             uint8_t nt_reserved,
                                             char *formatted_name) {
    bool base_lower = (nt_reserved & 0x08) != 0; // lowercase base name
    bool ext_lower  = (nt_reserved & 0x10) != 0; // lowercase extension

    int pos = 0;

    // Base name (first 8 chars, trimming trailing spaces)
    for (int i = 0; i < 8 && short_name[i] != ' '; i++) {
        char c = short_name[i];
        if (base_lower && c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
        formatted_name[pos++] = c;
    }

    // Detect if extension exists (any non-space in bytes 8-10)
    bool has_ext = false;
    for (int i = 8; i < 11; i++) {
        if (short_name[i] != ' ') { has_ext = true; break; }
    }

    if (has_ext) {
        formatted_name[pos++] = '.';
        for (int i = 8; i < 11 && short_name[i] != ' '; i++) {
            char c = short_name[i];
            if (ext_lower && c >= 'A' && c <= 'Z') {
                c = c - 'A' + 'a';
            }
            formatted_name[pos++] = c;
        }
    }

    formatted_name[pos] = '\0';
}

// ---- LFN (Long File Name) helpers -------------------------------------------

/**
 * Compute the 8.3 checksum used to validate LFN entry sequences.
 */
static uint8_t fat32_lfn_checksum(const char *short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short_name[i];
    }
    return sum;
}

/**
 * Extract UCS-2 characters from a single LFN entry and place them into
 * 'out' at the correct offset.  Each LFN entry stores 13 characters.
 * Characters beyond the name end are 0xFFFF.
 */
static void fat32_lfn_extract(const fat32_lfn_entry_t *lfn, char *out,
                              uint32_t max_len) {
    int seq = (lfn->order & 0x3F) - 1;  // 0-based sequence number
    int base = seq * 13;

    // 5 chars from name1, 6 from name2, 2 from name3
    uint16_t chars[13];
    for (int i = 0; i < 5; i++) chars[i]      = lfn->name1[i];
    for (int i = 0; i < 6; i++) chars[5 + i]  = lfn->name2[i];
    for (int i = 0; i < 2; i++) chars[11 + i] = lfn->name3[i];

    for (int i = 0; i < 13; i++) {
        uint32_t pos = base + i;
        if (pos >= max_len - 1) break;
        if (chars[i] == 0x0000 || chars[i] == 0xFFFF) {
            // null terminator or padding – leave as-is (caller zeroes buffer)
            if (chars[i] == 0x0000 && pos < max_len) {
                out[pos] = '\0';
            }
            continue;
        }
        // Downsample to ASCII (we only support basic characters)
        out[pos] = (chars[i] < 0x80) ? (char)chars[i] : '?';
    }
}

/**
 * Build LFN directory entries for creating a file with a long name.
 * 'short_name' is the corresponding 8.3 entry.
 * Returns the number of LFN entries written to 'lfn_entries' (caller
 * allocates space for up to 20 entries).
 */
static int fat32_lfn_build(const char *long_name, const char *short_name,
                           fat32_lfn_entry_t *lfn_entries) {
    uint8_t checksum = fat32_lfn_checksum(short_name);
    size_t name_len = strlen(long_name);
    int num_entries = (name_len + 12) / 13;  // ceiling division
    if (num_entries > 20) num_entries = 20;

    for (int seq = 0; seq < num_entries; seq++) {
        fat32_lfn_entry_t *lfn = &lfn_entries[num_entries - 1 - seq]; // stored in reverse
        memset(lfn, 0xFF, sizeof(fat32_lfn_entry_t));

        lfn->order = (uint8_t)(seq + 1);
        if (seq == num_entries - 1) {
            lfn->order |= 0x40;  // mark last LFN entry
        }
        lfn->attr = FAT32_ATTR_LFN;
        lfn->type = 0;
        lfn->checksum = checksum;
        lfn->first_cluster_lo = 0;

        int base = seq * 13;

        // Fill name1 (5 chars)
        for (int i = 0; i < 5; i++) {
            int pos = base + i;
            if ((size_t)pos < name_len) {
                lfn->name1[i] = (uint16_t)(uint8_t)long_name[pos];
            } else if ((size_t)pos == name_len) {
                lfn->name1[i] = 0x0000;  // null terminator
            } else {
                lfn->name1[i] = 0xFFFF;
            }
        }
        // Fill name2 (6 chars)
        for (int i = 0; i < 6; i++) {
            int pos = base + 5 + i;
            if ((size_t)pos < name_len) {
                lfn->name2[i] = (uint16_t)(uint8_t)long_name[pos];
            } else if ((size_t)pos == name_len) {
                lfn->name2[i] = 0x0000;
            } else {
                lfn->name2[i] = 0xFFFF;
            }
        }
        // Fill name3 (2 chars)
        for (int i = 0; i < 2; i++) {
            int pos = base + 11 + i;
            if ((size_t)pos < name_len) {
                lfn->name3[i] = (uint16_t)(uint8_t)long_name[pos];
            } else if ((size_t)pos == name_len) {
                lfn->name3[i] = 0x0000;
            } else {
                lfn->name3[i] = 0xFFFF;
            }
        }
    }

    return num_entries;
}

/* True when the name cannot live in a single 8.3 entry (or we want to keep
 * the original spelling, including lowercase, as an LFN). */
static bool fat32_needs_lfn(const char *name) {
    if (!name || !name[0]) return false;
    int name_len = 0, ext_len = 0;
    bool saw_dot = false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (saw_dot) return true;
            saw_dot = true;
            continue;
        }
        if (c <= 0x20 || c == '+' || c == ',' || c == ';' ||
            c == '=' || c == '[' || c == ']' || c == '*' ||
            c == '?' || c == '<' || c == '>' || c == '|' || c == ':')
            return true;
        if (c >= 'a' && c <= 'z') return true;
        if (saw_dot) {
            if (++ext_len > 3) return true;
        } else {
            if (++name_len > 8) return true;
        }
    }
    return name_len == 0;
}

/* A name that is a valid 8.3 name except for letter case, where the base
 * and the extension are each all lower or all upper case, is stored as a
 * plain 8.3 entry with the NT case bits (0x08 base lower, 0x10 extension
 * lower), the way Windows and mtools do. Returns true and sets *flags. */
static bool fat32_83_case_flags(const char *name, uint8_t *flags) {
    int name_len = 0, ext_len = 0;
    bool saw_dot = false;
    bool b_lo = false, b_up = false, e_lo = false, e_up = false;
    if (!name || !name[0] || name[0] == '.') return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (saw_dot) return false;
            saw_dot = true;
            continue;
        }
        if (c <= 0x20 || c >= 0x7F || c == '+' || c == ',' || c == ';' ||
            c == '=' || c == '[' || c == ']' || c == '*' || c == '?' ||
            c == '<' || c == '>' || c == '|' || c == ':' || c == '"' || c == '\\')
            return false;
        bool lo = (c >= 'a' && c <= 'z'), up = (c >= 'A' && c <= 'Z');
        if (saw_dot) {
            if (++ext_len > 3) return false;
            e_lo |= lo; e_up |= up;
        } else {
            if (++name_len > 8) return false;
            b_lo |= lo; b_up |= up;
        }
    }
    if (name_len == 0 || (saw_dot && ext_len == 0)) return false;
    if ((b_lo && b_up) || (e_lo && e_up)) return false;
    *flags = (uint8_t)((b_lo ? 0x08 : 0) | (e_lo ? 0x10 : 0));
    return true;
}

static char fat32_alias_char(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    if (c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' || c == '@' ||
        c == '~' || c == '`' || c == '!' || c == '(' || c == ')' || c == '{' ||
        c == '}' || c == '^' || c == '#' || c == '&')
        return c;
    return '_';
}

/* Windows-style numbered alias: up to 6 base characters, "~N", and the
 * first 3 characters of the last extension, unique within the directory
 * (STDIO_~1.H, STRESS~2.TXT, ...). One pass over the directory marks the
 * numbers already taken for this base/extension, then the lowest free one
 * is used, so creating many similar names stays linear per file. */
#define FAT32_ALIAS_MAX_N 4096u
static void fat32_make_short_alias(fat32_fs_t *fs, uint32_t dir_cluster,
                                   const char *long_name, char *short_name) {
    char base[8], ext[3];
    int bn = 0, en = 0;
    const char *dot = NULL;
    for (const char *p = long_name; *p; p++)
        if (*p == '.') dot = p;
    if (dot == long_name) dot = NULL;        /* ".profile": no extension */
    for (const char *p = long_name; *p && (!dot || p < dot) && bn < 8; p++) {
        if (*p == '.' || *p == ' ') continue;
        base[bn++] = fat32_alias_char(*p);
    }
    if (dot) {
        for (const char *p = dot + 1; *p && en < 3; p++) {
            if (*p == ' ' || *p == '.') continue;
            ext[en++] = fat32_alias_char(*p);
        }
    }
    if (bn == 0) base[bn++] = '_';
    char ext11[3] = { ' ', ' ', ' ' };
    memcpy(ext11, ext, (size_t)en);

    uint8_t *used = (uint8_t *)kalloc(FAT32_ALIAS_MAX_N / 8u);
    void *buf = kalloc(fs->bytes_per_cluster);
    if (used) memset(used, 0, FAT32_ALIAS_MAX_N / 8u);
    if (used && buf) {
        uint32_t epc = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);
        uint32_t c = dir_cluster, guard = 0;
        bool done = false;
        while (!done && c >= 2 && c != FAT32_CLUSTER_END && c != FAT32_CLUSTER_BAD &&
               guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
            if (fat32_read_cluster(fs, c, buf) != 0) break;
            fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
            for (uint32_t i = 0; i < epc; i++) {
                uint8_t n0 = (uint8_t)e[i].name[0];
                if (n0 == 0x00) break;
                if (n0 == 0xE5 || (e[i].attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN)
                    continue;
                if (memcmp(e[i].name + 8, ext11, 3) != 0) continue;
                int t = -1;
                for (int k = 1; k < 8; k++)
                    if (e[i].name[k] == '~') { t = k; break; }
                if (t < 0) continue;
                int pre = t < bn ? t : bn;
                if (pre != t || memcmp(e[i].name, base, (size_t)pre) != 0) continue;
                uint32_t v = 0;
                int k = t + 1;
                for (; k < 8 && e[i].name[k] >= '0' && e[i].name[k] <= '9'; k++)
                    v = v * 10u + (uint32_t)(e[i].name[k] - '0');
                if (k == t + 1) continue;
                for (; k < 8; k++) if (e[i].name[k] != ' ') break;
                if (k != 8) continue;
                if (v < FAT32_ALIAS_MAX_N) used[v / 8u] |= (uint8_t)(1u << (v % 8u));
            }
            uint32_t next = fat32_get_next_cluster(fs, c);
            if (next == FAT32_CLUSTER_END || next == FAT32_CLUSTER_BAD || next < 2) done = true;
            c = next;
        }
    }
    uint32_t n = 1;
    if (used) {
        while (n < FAT32_ALIAS_MAX_N - 1u && (used[n / 8u] & (1u << (n % 8u)))) n++;
    }
    {
        char num[8], tmp[8];
        int nl = 0, tl = 0;
        uint32_t v = n;
        do { tmp[tl++] = (char)('0' + v % 10u); v /= 10u; } while (v);
        num[nl++] = '~';
        while (tl) num[nl++] = tmp[--tl];
        int keep = bn;
        if (keep > 8 - nl) keep = 8 - nl;
        if (keep > 6) keep = 6;
        memset(short_name, ' ', 11);
        memcpy(short_name, base, (size_t)keep);
        memcpy(short_name + keep, num, (size_t)nl);
        memcpy(short_name + 8, ext11, 3);
    }
    if (used) kfree(used);
    if (buf) kfree(buf);
}

static void fat32_path_leaf(const char *path, char *out, size_t outsz) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    size_t n = 0;
    while (base[n] && n + 1 < outsz) {
        out[n] = base[n];
        n++;
    }
    out[n] = '\0';
}

// ---- End LFN helpers --------------------------------------------------------

// ---- Path resolution helpers ------------------------------------------------

/**
 * Case-insensitive string comparison for filename matching.
 */
static bool fat32_name_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

/**
 * Search a directory (starting at dir_cluster) for an entry matching
 * 'target_name_uc' (case-insensitive).  Matches against both the 8.3
 * short name and any assembled LFN.
 * If found, fills out_first_cluster with the entry's start cluster and
 * out_attr with its attributes.  Returns 0 on success, -1 if not found.
 */
static int fat32_find_in_dir_impl(fat32_fs_t *fs, uint32_t dir_cluster,
                             const char *target_name_uc,
                             uint32_t *out_first_cluster,
                             uint8_t  *out_attr) {
    void *buf = kalloc(fs->bytes_per_cluster);
    if (!buf) return -1;

    // LFN assembly state
    char lfn_buffer[MAX_FILENAME_LEN];
    bool lfn_active = false;
    uint8_t lfn_cksum = 0;

    uint32_t guard = 0;
    while (dir_cluster != FAT32_CLUSTER_END &&
           guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        kprof_add(KP_FAT_DIRSCAN, 1);
        if (fat32_read_cluster(fs, dir_cluster, buf) != 0) {
            kfree(buf);
            return -1;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
        uint32_t epc = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < epc; i++) {
            fat32_dir_entry_t *e = &entries[i];
            /* 0x00 = no more entries in THIS cluster. find_free_slots may place
             * new names in a later cluster when a trailing 0x00 run is shorter than
             * needed (e.g. one free slot but LFN+8.3 needs two). Stopping the whole
             * walk here made mkdir succeed while find/stat/ls missed the entry.
             * Continue the FAT chain like fat32_open() does. */
            if (((uint8_t)e->name[0]) == 0x00) { lfn_active = false; break; }
            if (((uint8_t)e->name[0]) == 0xE5) { lfn_active = false; continue; }

            // LFN entry
            if ((e->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)e;
                if (lfn->order & 0x40) {
                    memset(lfn_buffer, 0, sizeof(lfn_buffer));
                    lfn_active = true;
                    lfn_cksum = lfn->checksum;
                }
                if (lfn_active && lfn->checksum == lfn_cksum) {
                    fat32_lfn_extract(lfn, lfn_buffer, MAX_FILENAME_LEN);
                }
                continue;
            }

            if (e->attr & FAT32_ATTR_VOLUME_ID) { lfn_active = false; continue; }

            // Regular entry – check LFN match first, then 8.3
            bool matched = false;

            if (lfn_active) {
                uint8_t actual_ck = fat32_lfn_checksum(e->name);
                if (actual_ck == lfn_cksum && lfn_buffer[0] != '\0') {
                    if (fat32_name_eq_ci(lfn_buffer, target_name_uc)) {
                        matched = true;
                    }
                }
                lfn_active = false;
            }

            if (!matched) {
                char formatted[256];
                fat32_format_83_filename(e->name, formatted);
                if (fat32_name_eq_ci(formatted, target_name_uc)) {
                    matched = true;
                }
            }

            if (matched) {
                if (out_first_cluster) {
                    *out_first_cluster = ((uint32_t)e->first_cluster_hi << 16) |
                                          e->first_cluster_lo;
                }
                if (out_attr) *out_attr = e->attr;
                kfree(buf);
                return 0;
            }
        }
        dir_cluster = fat32_get_next_cluster(fs, dir_cluster);
    }

    kfree(buf);
    return -1;
}

static int fat32_find_in_dir(fat32_fs_t *fs, uint32_t dir_cluster,
                             const char *target_name_uc,
                             uint32_t *out_first_cluster,
                             uint8_t  *out_attr) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_find_in_dir_impl(fs, dir_cluster, target_name_uc, out_first_cluster, out_attr);
    kprof_end(KP_FAT_FIND, kp_t0, 0);
    return kp_r;
}

/**
 * Resolve a full path (e.g. "/docs/notes/readme.txt") to:
 *   - *out_dir_cluster : cluster of the parent directory ("notes")
 *   - out_basename     : "README.TXT" (the last component, upper-cased)
 *
 * Returns 0 on success, -1 if any intermediate directory is missing or
 * is not actually a directory.
 */
static int fat32_resolve_path_impl(const char *path, uint32_t *out_dir_cluster,
                       char *out_basename, uint32_t basename_max) {
    if (!fat32_is_mounted() || !path || !out_dir_cluster || !out_basename) {
        return -1;
    }

    // Skip leading slash
    while (*path == '/') path++;

    /* "./wad" or "/./wad" — POSIX cwd; FAT32 has no per-process cwd here */
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
        while (*path == '/') path++;
    }

    // If path is empty after stripping, it refers to root itself
    if (*path == '\0') {
        *out_dir_cluster = fat32_filesystem.root_dir_cluster;
        out_basename[0] = '\0';
        return 0;
    }

    uint32_t current_cluster = fat32_filesystem.root_dir_cluster;

    // Tokenize the path by '/'
    char path_copy[MAX_PATH_LEN];
    size_t plen = strlen(path);
    if (plen >= MAX_PATH_LEN) plen = MAX_PATH_LEN - 1;
    memmove(path_copy, path, plen);
    path_copy[plen] = '\0';

    // Find the last component (basename)
    char *last_slash = NULL;
    for (int i = (int)plen - 1; i >= 0; i--) {
        if (path_copy[i] == '/') {
            last_slash = &path_copy[i];
            break;
        }
    }

    char *basename_start;
    if (last_slash) {
        *last_slash = '\0';       // terminate the directory portion
        basename_start = last_slash + 1;
    } else {
        // Only one component – parent is root
        basename_start = path_copy;
        *out_dir_cluster = fat32_filesystem.root_dir_cluster;
    }

    // Walk intermediate directory components
    if (last_slash) {
        char *component = path_copy;
        while (*component) {
            // Skip consecutive slashes
            while (*component == '/') component++;
            if (*component == '\0') break;

            // Find end of this component
            char *next = component;
            while (*next && *next != '/') next++;
            bool more = (*next == '/');
            if (more) *next = '\0';

            if (component[0] == '.' && component[1] == '\0') {
                component = more ? next + 1 : next;
                continue;
            }

            // Upper-case the component for comparison
            char comp_uc[256];
            size_t clen = strlen(component);
            if (clen >= sizeof(comp_uc)) clen = sizeof(comp_uc) - 1;
            for (size_t ci = 0; ci < clen; ci++) {
                char c = component[ci];
                if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
                comp_uc[ci] = c;
            }
            comp_uc[clen] = '\0';

            uint32_t child_cluster = 0;
            uint8_t  child_attr = 0;
            if (fat32_find_in_dir(&fat32_filesystem, current_cluster,
                                  comp_uc, &child_cluster, &child_attr) != 0) {
                return -1;  // component not found
            }
            if (!(child_attr & FAT32_ATTR_DIRECTORY)) {
                return -1;  // component is not a directory
            }
            current_cluster = child_cluster;

            component = more ? next + 1 : next;
        }
        *out_dir_cluster = current_cluster;
    }

    // Upper-case the basename into the output buffer
    size_t blen = strlen(basename_start);
    if (blen >= basename_max) blen = basename_max - 1;
    for (size_t i = 0; i < blen; i++) {
        char c = basename_start[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        out_basename[i] = c;
    }
    out_basename[blen] = '\0';

    return 0;
}

int fat32_resolve_path(const char *path, uint32_t *out_dir_cluster,
                       char *out_basename, uint32_t basename_max) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_resolve_path_impl(path, out_dir_cluster, out_basename, basename_max);
    kprof_end(KP_FAT_RESOLVE, kp_t0, 0);
    return kp_r;
}

// ---- End path resolution helpers --------------------------------------------

int fat32_mount(uint8_t storage_device_id) {
    FS_LOCK();
    DEBUG_ENTER_ARGS("device_id=%d", storage_device_id);
    memset(&fat32_filesystem, 0, sizeof(fat32_filesystem));
    fat32_init_file_table();
    
    storage_device_t *dev = storage_get_device(storage_device_id);
    if (!dev) {
        printf("fat32: Storage device not found\n");
        FS_UNLOCK(); return -1;
    }
    fat32_filesystem.storage_dev = dev;
    if (!fat32_filesystem.storage_dev) {
        printf("fat32: storage_dev is NULL after assignment!\n");
        FS_UNLOCK(); return -1;
    }
    // Find partition offset
    extern uint32_t find_first_fat32_partition_lba(uint8_t device_id);
    uint32_t partition_lba = find_first_fat32_partition_lba(storage_device_id);
    printf("fat32_mount: Using partition LBA offset %u\n", partition_lba);
    // Read boot sector from partition start
    static uint8_t boot_sector_buffer[SECTOR_SIZE];
    memset(boot_sector_buffer, 0, sizeof(boot_sector_buffer));
    if (storage_read_sectors(storage_device_id, partition_lba, 1, boot_sector_buffer) != 0) {
        printf("fat32: Failed to read boot sector at LBA %u\n", partition_lba);
        FS_UNLOCK(); return -1;
    }
    fat32_boot_sector_t *boot = (fat32_boot_sector_t*)boot_sector_buffer;
    if (!boot) {
        printf("fat32: boot pointer is NULL!\n");
        FS_UNLOCK(); return -1;
    }
    memmove(&fat32_filesystem.boot_sector, boot, sizeof(fat32_boot_sector_t));
    // Validate FAT32 signature
    if (boot->boot_signature != FAT32_SIGNATURE) {
        printf("fat32: Invalid boot signature (0x%x) at LBA %u. This device may not be formatted as FAT32. Please format the drive as FAT32 and try again.\n", boot->boot_signature, partition_lba);
        FS_UNLOCK(); return -1;
    }
    // Calculate filesystem layout
    fat32_filesystem.sectors_per_cluster = boot->sectors_per_cluster;
    fat32_filesystem.bytes_per_cluster = boot->bytes_per_sector * boot->sectors_per_cluster;
    fat32_filesystem.fat_start_sector = boot->reserved_sector_count + partition_lba; // Adjust for partition offset
    fat32_filesystem.data_start_sector = boot->reserved_sector_count + 
                                       (boot->num_fats * boot->fat_size_32) + partition_lba;
    fat32_filesystem.root_dir_cluster = boot->root_cluster;
    printf("fat32: Filesystem layout:\n");
    printf("  Bytes per sector: %d\n", boot->bytes_per_sector);
    printf("  Sectors per cluster: %d\n", boot->sectors_per_cluster);
    printf("  FAT start sector: %d\n", fat32_filesystem.fat_start_sector);
    printf("  Data start sector: %d\n", fat32_filesystem.data_start_sector);
    printf("  Root directory cluster: %d\n", fat32_filesystem.root_dir_cluster);
    /* Do not read the FAT. A 32 GB volume's FAT is megabytes; mount and ls
     * only need the sectors a lookup touches. fat_table stays NULL. */
    fat32_filesystem.fat_table = NULL;
    fat32_fat_cache_reset();
    fat_dirty_lo = 0xFFFFFFFF;
    fat_dirty_hi = 0;
    fat_alloc_cursor = 2;
    fat_meta_changed = false;
    fat_create_hint.valid = false;
    if (boot->fs_info != 0 && boot->fs_info != 0xFFFF) {
        uint8_t info_buf[512];
        if (storage_read_sectors(storage_device_id, partition_lba + boot->fs_info,
                                 1, info_buf) == 0) {
            fat32_fsinfo_t *fi = (fat32_fsinfo_t *)info_buf;
            uint32_t bps = boot->bytes_per_sector ? boot->bytes_per_sector : 512;
            uint32_t max_entries = (uint32_t)(((uint64_t)boot->fat_size_32 * bps) / 4);
            if (fi->lead_signature == 0x41615252u &&
                fi->struct_signature == 0x61417272u &&
                fi->next_free >= 2 && fi->next_free < max_entries) {
                fat_alloc_cursor = fi->next_free;
            }
        }
    }
    fat_cache_live = true;
    printf("fat32: FAT is %u sectors (%u KiB), cached in %u-sector windows (not loaded)\n",
           boot->fat_size_32,
           (uint32_t)(((uint64_t)boot->fat_size_32 * (boot->bytes_per_sector ? boot->bytes_per_sector : 512)) / 1024),
           FAT32_FAT_CACHE_SLOTS);
    extern void storage_bcache_invalidate_all(void);
    storage_bcache_invalidate_all();
    fat32_filesystem.is_mounted = true;
    printf("fat32: Filesystem mounted successfully\n");
    FS_UNLOCK(); return 0;
}

void fat32_unmount() {
    FS_LOCK();
    if (fat_cache_live)
        fat32_sync_locked();
    fat_defer_flush = false;
    fat_meta_changed = false;
    fat_create_hint.valid = false;
    fat32_fat_cache_reset();
    if (fat32_filesystem.fat_table) {
        kfree(fat32_filesystem.fat_table);
    }
    memset(&fat32_filesystem, 0, sizeof(fat32_filesystem));
    extern void storage_bcache_invalidate_all(void);
    storage_bcache_invalidate_all();
    printf("fat32: Filesystem unmounted\n");
    FS_UNLOCK();
}

bool fat32_is_mounted() {
    return fat32_filesystem.is_mounted;
}

// List the contents of a directory given its start cluster.
// Assembles long file names from LFN entries when available.
// Caller must hold fat32_lock (e.g. fat32_list_directory); do not FS_LOCK here
// or nested spinlocks deadlock.
int fat32_list_directory_cluster(uint32_t dir_cluster,
                                 fat32_dir_list_t *entries,
                                 uint32_t max_entries) {
    if (!fat32_is_mounted() || !entries)
        return -1;

    uint32_t entry_count = 0;
    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer)
        return -1;

    // LFN assembly state
    char lfn_buffer[MAX_FILENAME_LEN];
    bool lfn_active = false;
    uint8_t lfn_checksum_expected = 0;

    uint32_t guard = 0;
    while (dir_cluster != FAT32_CLUSTER_END && entry_count < max_entries &&
           guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return -1;
        }
        fat32_dir_entry_t *dir_entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t epc = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < epc && entry_count < max_entries; i++) {
            fat32_dir_entry_t *e = &dir_entries[i];
            if (((uint8_t)e->name[0]) == 0x00) {
                lfn_active = false;
                break; /* end of this cluster; follow chain for later entries */
            }
            if (((uint8_t)e->name[0]) == 0xE5) {
                lfn_active = false;
                continue;
            }

            // Handle LFN entries
            if ((e->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)e;
                if (lfn->order & 0x40) {
                    // First (last-stored) LFN entry – start a new sequence
                    memset(lfn_buffer, 0, sizeof(lfn_buffer));
                    lfn_active = true;
                    lfn_checksum_expected = lfn->checksum;
                }
                if (lfn_active && lfn->checksum == lfn_checksum_expected) {
                    fat32_lfn_extract(lfn, lfn_buffer, MAX_FILENAME_LEN);
                }
                continue;
            }

            // Skip volume label
            if (e->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_active = false;
                continue;
            }

            // Regular short-name entry – use assembled LFN if valid
            if (lfn_active) {
                uint8_t actual_cksum = fat32_lfn_checksum(e->name);
                if (actual_cksum == lfn_checksum_expected && lfn_buffer[0] != '\0') {
                    // Use the long file name
                    size_t ln = strlen(lfn_buffer);
                    if (ln >= MAX_FILENAME_LEN) ln = MAX_FILENAME_LEN - 1;
                    memmove(entries[entry_count].name, lfn_buffer, ln);
                    entries[entry_count].name[ln] = '\0';
                } else {
                    // Checksum mismatch – fall back to 8.3
                    fat32_format_83_filename_display(e->name, e->nt_reserved,
                                                     entries[entry_count].name);
                }
                lfn_active = false;
            } else {
                fat32_format_83_filename_display(e->name, e->nt_reserved,
                                                 entries[entry_count].name);
            }

            entries[entry_count].name[MAX_FILENAME_LEN-1] = '\0';
            entries[entry_count].size = e->file_size;
            entries[entry_count].attributes = e->attr;
            entries[entry_count].is_directory = (e->attr & FAT32_ATTR_DIRECTORY) != 0;
            entry_count++;
        }
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }

    kfree(cluster_buffer);
    return entry_count;
}

int fat32_list_directory(const char *path, fat32_dir_list_t *entries, uint32_t max_entries) {
    FS_LOCK();
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        FS_UNLOCK(); return -1;
    }
    if (!entries) {
        printf("fat32_list_directory: entries pointer is NULL!\n");
        FS_UNLOCK(); return -1;
    }

    // Resolve path to directory cluster
    uint32_t dir_cluster = fat32_filesystem.root_dir_cluster;

    if (path && path[0] != '\0') {
        // Strip leading/trailing slashes and check if the path names a subdir
        const char *p = path;
        while (*p == '/') p++;

        if (*p != '\0') {
            // The path names a directory – resolve it
            // Use resolve_path to walk to the named directory
            uint32_t parent_cluster = 0;
            char basename[256];
            if (fat32_resolve_path(path, &parent_cluster, basename, sizeof(basename)) != 0) {
                printf("fat32: Path '%s' not found\n", path);
                FS_UNLOCK(); return -1;
            }

            if (basename[0] == '\0') {
                // Path ended in '/' – parent_cluster IS the directory to list
                dir_cluster = parent_cluster;
            } else {
                // basename is a directory name inside parent_cluster – look it up
                uint32_t child_cluster = 0;
                uint8_t  child_attr = 0;
                if (fat32_find_in_dir(&fat32_filesystem, parent_cluster,
                                      basename, &child_cluster, &child_attr) != 0) {
                    printf("fat32: '%s' not found\n", path);
                    FS_UNLOCK(); return -1;
                }
                if (!(child_attr & FAT32_ATTR_DIRECTORY)) {
                    printf("fat32: '%s' is not a directory\n", path);
                    FS_UNLOCK(); return -1;
                }
                dir_cluster = child_cluster;
            }
        }
    }

    int result = fat32_list_directory_cluster(dir_cluster, entries, max_entries);
    FS_UNLOCK(); return result;
}

static fat32_file_t* fat32_open_impl(const char *filename, const char *mode) {
    FS_LOCK();
    DEBUG_ENTER_ARGS("filename='%s', mode='%s'", filename, mode);
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        DEBUG_POINT("Filesystem not mounted - returning NULL");
        FS_UNLOCK(); return NULL;
    }

    // Resolve path to parent directory + basename
    uint32_t parent_cluster = 0;
    char basename_uc[256];
    /* The file fat32_create_file() just placed: skip the path walk. */
    bool hint_ok = fat_create_hint.valid && filename &&
                   strcmp(filename, fat_create_hint.path) == 0;
    if (hint_ok) {
        parent_cluster = fat_create_hint.parent;
    } else {
        fat_create_hint.valid = false;
        if (fat32_resolve_path(filename, &parent_cluster, basename_uc, sizeof(basename_uc)) != 0) {
            printf("fat32: Path resolution failed for '%s'\n", filename);
            FS_UNLOCK(); return NULL;
        }
        if (basename_uc[0] == '\0') {
            /* "dir/" names a directory; sys_open falls back to a directory fd. */
            FS_UNLOCK(); return NULL;
        }
    }

    // Find free file handle
    fat32_file_t *file = NULL;
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            file = &open_files[i];
            break;
        }
    }
    if (!file) {
        printf("fat32: No free file handles\n");
        FS_UNLOCK(); return NULL;
    }

    // Search for file in the resolved parent directory
    uint32_t dir_cluster = parent_cluster;
    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (hint_ok && cluster_buffer) {
        fat_create_hint.valid = false;
        uint32_t epc0 = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);
        if (fat_create_hint.index < epc0 &&
            fat32_read_cluster(&fat32_filesystem, fat_create_hint.cluster, cluster_buffer) == 0) {
            fat32_dir_entry_t *he = &((fat32_dir_entry_t *)cluster_buffer)[fat_create_hint.index];
            uint8_t hn0 = (uint8_t)he->name[0];
            if (hn0 != 0x00 && hn0 != 0xE5 &&
                (he->attr & FAT32_ATTR_LFN) != FAT32_ATTR_LFN &&
                !(he->attr & FAT32_ATTR_VOLUME_ID) &&
                memcmp(he->name, fat_create_hint.name, 11) == 0) {
                file->in_use = true;
                file->file_size = he->file_size;
                file->file_position = 0;
                file->attributes = he->attr;
                file->filesystem = &fat32_filesystem;
                file->parent_dir_cluster = parent_cluster;
                size_t hfl = strlen(filename);
                if (hfl >= sizeof(file->filename)) hfl = sizeof(file->filename) - 1;
                memmove(file->filename, filename, hfl);
                file->filename[hfl] = '\0';
                file->first_cluster = ((uint32_t)he->first_cluster_hi << 16) | he->first_cluster_lo;
                file->current_cluster = file->first_cluster;
                file->dirent_cluster = fat_create_hint.cluster;
                file->dirent_index = fat_create_hint.index;
                memcpy(file->dirent_name, he->name, 11);
                kfree(cluster_buffer);
                FS_UNLOCK(); return file;
            }
        }
    }
    if (hint_ok) {
        /* Stale hint: fall back to the normal lookup. */
        if (fat32_resolve_path(filename, &parent_cluster, basename_uc, sizeof(basename_uc)) != 0 ||
            basename_uc[0] == '\0') {
            if (cluster_buffer) kfree(cluster_buffer);
            FS_UNLOCK(); return NULL;
        }
        dir_cluster = parent_cluster;
    }
    if (!cluster_buffer) {
        printf("fat32: Failed to allocate cluster buffer\n");
        FS_UNLOCK(); return NULL;
    }

    // LFN state for scanning
    char open_lfn_buf[MAX_FILENAME_LEN];
    bool open_lfn_active = false;
    uint8_t open_lfn_cksum = 0;

    uint32_t chain_guard = 0;
    while (dir_cluster != FAT32_CLUSTER_END &&
           chain_guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            FS_UNLOCK(); return NULL;
        }

        fat32_dir_entry_t *dir_entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *entry = &dir_entries[i];

            if (((uint8_t)entry->name[0]) == 0x00) break;
            if (((uint8_t)entry->name[0]) == 0xE5) { open_lfn_active = false; continue; }

            // Accumulate LFN entries
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)entry;
                if (lfn->order & 0x40) {
                    memset(open_lfn_buf, 0, sizeof(open_lfn_buf));
                    open_lfn_active = true;
                    open_lfn_cksum = lfn->checksum;
                }
                if (open_lfn_active && lfn->checksum == open_lfn_cksum) {
                    fat32_lfn_extract(lfn, open_lfn_buf, MAX_FILENAME_LEN);
                }
                continue;
            }
            if (entry->attr & FAT32_ATTR_VOLUME_ID) { open_lfn_active = false; continue; }

            // Check LFN match first, then 8.3
            bool matched = false;
            if (open_lfn_active) {
                uint8_t ck = fat32_lfn_checksum(entry->name);
                if (ck == open_lfn_cksum && open_lfn_buf[0] != '\0') {
                    if (fat32_name_eq_ci(open_lfn_buf, basename_uc)) matched = true;
                }
                open_lfn_active = false;
            }
            if (!matched) {
                static char formatted_name[256];
                fat32_format_83_filename(entry->name, formatted_name);
                if (fat32_name_eq_ci(formatted_name, basename_uc)) matched = true;
            }

            if (matched) {
                file->in_use = true;
                file->file_size = entry->file_size;
                file->file_position = 0;
                file->attributes = entry->attr;
                file->filesystem = &fat32_filesystem;
                file->parent_dir_cluster = parent_cluster;
                size_t fn_len = strlen(filename);
                if (fn_len >= sizeof(file->filename)) {
                    fn_len = sizeof(file->filename) - 1;
                }
                memmove(file->filename, filename, fn_len);
                file->filename[fn_len] = '\0';

                file->first_cluster = ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
                file->current_cluster = file->first_cluster;
                file->dirent_cluster = dir_cluster;
                file->dirent_index = i;
                memcpy(file->dirent_name, entry->name, 11);

                kfree(cluster_buffer);
                ktrace("fat32: Opened file '%s' (size: %d bytes, cluster: %d)\n",
                       filename, file->file_size, file->first_cluster);
                FS_UNLOCK(); return file;
            }
        }

        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }

    kfree(cluster_buffer);
    printf("fat32: File '%s' not found\n", filename);
    FS_UNLOCK(); return NULL;
}

fat32_file_t* fat32_open(const char *filename, const char *mode) {
    uint64_t kp_t0 = kprof_begin();
    fat32_file_t* kp_r = fat32_open_impl(filename, mode);
    kprof_end(KP_FAT_OPEN, kp_t0, 0);
    return kp_r;
}

int fat32_close(fat32_file_t *file) {
    FS_LOCK();
    if (!file || !file->in_use) {
        FS_UNLOCK(); return -1;
    }
    
    file->in_use = false;
    /* End of a file: write back the FAT sectors its writes dirtied. */
    int rc = 0;
    if (!fat_defer_flush && fat_dirty_lo <= fat_dirty_hi &&
        fat_cache_live && fat32_filesystem.is_mounted)
        rc = fat32_flush_fat(&fat32_filesystem);
    FS_UNLOCK(); return rc;
}

/* Max sectors per storage_read_sectors call (matches storage.c batch size).
 * 256 is the ATA PIO READ SECTORS limit (128 KiB). */
#define FAT32_READ_IO_SECTORS 1024

#define FAT32_WRITE_IO_SECTORS 1024u
static void *fat32_write_io_buf;

static void *fat32_read_io_buf;

static int fat32_read_impl(fat32_file_t *file, void *buffer, uint32_t size) {
    FS_LOCK();
    if (!file || !file->in_use) {
        FS_UNLOCK(); return -1;
    }
    
    if (file->file_position >= file->file_size) {
        FS_UNLOCK(); return 0; // EOF
    }
    
    // Limit read size to remaining bytes in file
    uint32_t remaining_bytes = file->file_size - file->file_position;
    if (size > remaining_bytes) {
        size = remaining_bytes;
    }
    
    fat32_fs_t *fs = file->filesystem;
    uint32_t bpc = fs->bytes_per_cluster;
    uint32_t spc = fs->sectors_per_cluster;
    if (bpc == 0 || spc == 0) {
        FS_UNLOCK(); return -1;
    }

    uint32_t bytes_read = 0;
    uint8_t *output_buffer = (uint8_t*)buffer;

    uint32_t runs = 0;
    uint64_t read_start_us = ktime_now_us();

    if (!fat32_read_io_buf) {
        fat32_read_io_buf = kalloc(FAT32_READ_IO_SECTORS * 512);
        if (!fat32_read_io_buf) {
            FS_UNLOCK(); return -1;
        }
    }
    void *io_buf = fat32_read_io_buf;

    while (bytes_read < size &&
           file->current_cluster >= 2 &&
           file->current_cluster != FAT32_CLUSTER_END &&
           file->current_cluster != FAT32_CLUSTER_BAD) {

        uint32_t cluster = file->current_cluster;
        uint32_t start_sector = fat32_cluster_to_sector(fs, cluster);
        if (start_sector == 0)
            break;

        /*
         * Cap the I/O run to what this read still needs (+ small readahead).
         * The old path always pulled up to 64 KB even for a 64-byte lump,
         * which thrashed the block cache and forced AHCI on every WAD miss.
         */
        uint32_t cluster_offset_peek = file->file_position % bpc;
        uint32_t need_bytes = (size - bytes_read) + cluster_offset_peek;
        uint32_t want_sectors = (need_bytes + 511u) / 512u + 8u; /* ~4 KB readahead */
        if (want_sectors > FAT32_READ_IO_SECTORS)
            want_sectors = FAT32_READ_IO_SECTORS;
        uint32_t max_run_clusters = want_sectors / spc;
        if (max_run_clusters < 1)
            max_run_clusters = 1;

        /* Coalesce physically contiguous clusters into one disk read. */
        uint32_t run_clusters = 1;
        uint32_t tail = cluster;
        while (run_clusters < max_run_clusters) {
            uint32_t next = fat32_get_next_cluster(fs, tail);
            if (next < 2 || next >= FAT32_CLUSTER_END)
                break;
            uint32_t expected = start_sector + run_clusters * spc;
            if (fat32_cluster_to_sector(fs, next) != expected)
                break;
            tail = next;
            run_clusters++;
        }

        uint32_t run_sectors = run_clusters * spc;
        runs++;
        if (storage_read_sectors(fs->storage_dev->device_id, start_sector,
                               run_sectors, io_buf) != 0) {
            FS_UNLOCK();
            return bytes_read > 0 ? (int)bytes_read : -1;
        }

        uint32_t cluster_offset = file->file_position % bpc;
        uint32_t avail = run_clusters * bpc - cluster_offset;
        uint32_t to_copy = size - bytes_read;
        if (to_copy > avail)
            to_copy = avail;

        memcpy(output_buffer + bytes_read,
               (uint8_t *)io_buf + cluster_offset, to_copy);
        bytes_read += to_copy;
        file->file_position += to_copy;

        uint32_t consumed = cluster_offset + to_copy;
        if (consumed >= run_clusters * bpc) {
            file->current_cluster = fat32_get_next_cluster(fs, tail);
        } else {
            uint32_t adv = consumed / bpc;
            uint32_t c = cluster;
            for (uint32_t i = 0; i < adv; i++)
                c = fat32_get_next_cluster(fs, c);
            file->current_cluster = c;
        }
    }

    FS_UNLOCK();
    /* Diagnostic: how fragmented is the read?  One HW command is issued per
     * physically contiguous run, so the run count explains slow program loads
     * (an AHCI command costs ~10-25 ms under emulation).  Only reported when
     * the read was actually slow, to keep the log quiet. */
    if (size >= 4096 && runs > 0) {
        uint64_t spent_us = ktime_now_us() - read_start_us;
        if (spent_us > 5000) {
            serial_printf("fat32: read %u bytes from '%s' in %u run(s), %u us\n",
                          bytes_read, file->filename, runs, (uint32_t)spent_us);
        }
    }
    return bytes_read;
}

int fat32_read(fat32_file_t *file, void *buffer, uint32_t size) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_read_impl(file, buffer, size);
    kprof_end(KP_FAT_READ, kp_t0, 0);
    return kp_r;
}

/**
 * Extend (or create) the cluster chain for a file so that it contains at
 * least 'needed_clusters' clusters.  Returns 0 on success, -1 on failure.
 */
static int fat32_ensure_clusters(fat32_file_t *file, uint32_t needed_clusters) {
    fat32_fs_t *fs = file->filesystem;

    // Allocate the first cluster if the file has none
    if (file->first_cluster == 0 || file->first_cluster == FAT32_CLUSTER_END) {
        uint32_t c = fat32_allocate_cluster(fs);
        if (c == FAT32_CLUSTER_END) return -1;
        file->first_cluster   = c;
        file->current_cluster = c;
    }

    // Walk to the end of the current chain and count existing clusters
    uint32_t count = 1;
    uint32_t last  = file->first_cluster;
    while (count < needed_clusters) {
        uint32_t next = fat32_get_next_cluster(fs, last);
        if (next == FAT32_CLUSTER_END || next == FAT32_CLUSTER_BAD ||
            next == FAT32_CLUSTER_FREE) {
            break;
        }
        last = next;
        count++;
    }

    // Allocate additional clusters if needed
    while (count < needed_clusters) {
        uint32_t c = fat32_allocate_cluster(fs);
        if (c == FAT32_CLUSTER_END) {
            printf("fat32: Out of space while extending file\n");
            return -28;   /* -ENOSPC */
        }
        // Link the new cluster to the chain
        if (fat32_fat_store(fs, last, c) != 0)
            return -1;
        last = c;
        count++;
    }

    /* The FAT is written back at fat32_close() (or fat32_sync() when the
     * caller deferred it), not after every write: each flush is a write per
     * FAT copy, which dominated small-file copies. */
    return 0;
}

/**
 * Update the directory entry for an open file (first_cluster and file_size).
 * Searches from dir_start_cluster (typically root or parent directory).
 */
static int fat32_update_dir_entry_impl(fat32_file_t *file, uint32_t dir_start_cluster) {
    fat32_fs_t *fs = file->filesystem;
    void *dir_buf = kalloc(fs->bytes_per_cluster);
    if (!dir_buf) return -1;

    /* Known slot: rewrite that one cluster after checking the slot still
     * holds this file's 8.3 entry (not deleted, renamed or reused). */
    if (file->dirent_cluster >= 2 &&
        file->dirent_index < fs->bytes_per_cluster / sizeof(fat32_dir_entry_t) &&
        fat32_read_cluster(fs, file->dirent_cluster, dir_buf) == 0) {
        fat32_dir_entry_t *e = &((fat32_dir_entry_t *)dir_buf)[file->dirent_index];
        uint8_t n0 = (uint8_t)e->name[0];
        if (n0 != 0x00 && n0 != 0xE5 &&
            (e->attr & FAT32_ATTR_LFN) != FAT32_ATTR_LFN &&
            !(e->attr & (FAT32_ATTR_VOLUME_ID | FAT32_ATTR_DIRECTORY)) &&
            memcmp(e->name, file->dirent_name, 11) == 0) {
            e->first_cluster_hi = (uint16_t)(file->first_cluster >> 16);
            e->first_cluster_lo = (uint16_t)(file->first_cluster & 0xFFFF);
            e->file_size        = file->file_size;
            int ret = fat32_write_cluster(fs, file->dirent_cluster, dir_buf);
            kfree(dir_buf);
            return ret;
        }
        file->dirent_cluster = 0;
    }

    // Use basename only (dir entries store "NAME.EXT", not "/path/NAME.EXT")
    const char *base = file->filename;
    const char *last_slash = base;
    for (const char *p = base; *p; p++)
        if (*p == '/') last_slash = p + 1;
    base = last_slash;

    // Build upper-case basename for matching
    char filename_uc[256];
    size_t fn_len = strlen(base);
    if (fn_len >= sizeof(filename_uc)) fn_len = sizeof(filename_uc) - 1;
    for (size_t i = 0; i < fn_len; i++) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        filename_uc[i] = c;
    }
    filename_uc[fn_len] = '\0';

    // LFN assembly state (files created with long names have no matching
    // 8.3 alias the caller knows about — must match the assembled LFN too,
    // otherwise writes to long-named files never update size/cluster).
    char lfn_buffer[MAX_FILENAME_LEN];
    bool lfn_active = false;
    uint8_t lfn_cksum = 0;

    uint32_t dir_cluster = dir_start_cluster;
    uint32_t guard = 0;
    while (dir_cluster != FAT32_CLUSTER_END &&
           guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        if (fat32_read_cluster(fs, dir_cluster, dir_buf) != 0) {
            kfree(dir_buf);
            return -1;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)dir_buf;
        uint32_t epc = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < epc; i++) {
            fat32_dir_entry_t *e = &entries[i];
            if (((uint8_t)e->name[0]) == 0x00) break;
            if (((uint8_t)e->name[0]) == 0xE5) { lfn_active = false; continue; }

            if ((e->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)e;
                if (lfn->order & 0x40) {
                    memset(lfn_buffer, 0, sizeof(lfn_buffer));
                    lfn_active = true;
                    lfn_cksum = lfn->checksum;
                }
                if (lfn_active && lfn->checksum == lfn_cksum) {
                    fat32_lfn_extract(lfn, lfn_buffer, MAX_FILENAME_LEN);
                }
                continue;
            }

            if (e->attr & FAT32_ATTR_VOLUME_ID) { lfn_active = false; continue; }

            bool matched = false;
            if (lfn_active) {
                uint8_t actual_ck = fat32_lfn_checksum(e->name);
                if (actual_ck == lfn_cksum && lfn_buffer[0] != '\0' &&
                    fat32_name_eq_ci(lfn_buffer, filename_uc)) {
                    matched = true;
                }
                lfn_active = false;
            }
            if (!matched) {
                char formatted[256];
                fat32_format_83_filename(e->name, formatted);
                if (fat32_name_eq_ci(formatted, filename_uc)) {
                    matched = true;
                }
            }

            if (matched) {
                e->first_cluster_hi = (uint16_t)(file->first_cluster >> 16);
                e->first_cluster_lo = (uint16_t)(file->first_cluster & 0xFFFF);
                e->file_size        = file->file_size;
                file->dirent_cluster = dir_cluster;
                file->dirent_index = i;
                memcpy(file->dirent_name, e->name, 11);

                int ret = fat32_write_cluster(fs, dir_cluster, dir_buf);
                kfree(dir_buf);
                return ret;
            }
        }
        dir_cluster = fat32_get_next_cluster(fs, dir_cluster);
    }

    kfree(dir_buf);
    printf("fat32: dir entry not found for '%s'\n", file->filename);
    return -1;
}

static int fat32_update_dir_entry(fat32_file_t *file, uint32_t dir_start_cluster) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_update_dir_entry_impl(file, dir_start_cluster);
    kprof_end(KP_FAT_UPDDIR, kp_t0, 0);
    return kp_r;
}

static int fat32_write_impl(fat32_file_t *file, const void *buffer, uint32_t size) {
    FS_LOCK();
    SERIAL_DEBUG_PRINT(FAT32, "[fat32_write] file='%s' size=%d pos=%d\n",
                       file ? file->filename : "(null)", (int)size,
                       file ? (int)file->file_position : 0);

    if (!file || !file->in_use || !buffer) { FS_UNLOCK(); return -1; }
    if (size == 0) { FS_UNLOCK(); return 0; }

    fat32_fs_t *fs = file->filesystem;
    uint32_t bpc = fs->bytes_per_cluster;

    // Compute how many total clusters the file needs after this write
    uint32_t end_pos = file->file_position + size;
    uint32_t needed_clusters = (end_pos + bpc - 1) / bpc;
    if (needed_clusters == 0) needed_clusters = 1;

    // Ensure enough clusters are allocated
    int ec = fat32_ensure_clusters(file, needed_clusters);
    if (ec != 0) {
        serial_printf("fat32: write '%s' pos=%u size=%u: cluster alloc failed (%d)\n",
                      file->filename, (unsigned)file->file_position, (unsigned)size, ec);
        FS_UNLOCK(); return ec == -28 ? -28 /* -ENOSPC */ : -5 /* -EIO */;
    }

    // Allocate one cluster buffer for read-modify-write
    void *cluster_buf = kalloc(bpc);
    if (!cluster_buf) {
        printf("fat32: write alloc failed\n");
        FS_UNLOCK(); return -1;
    }

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t bytes_written = 0;

    // Navigate to the cluster corresponding to file_position
    uint32_t cluster_index = file->file_position / bpc;
    uint32_t cluster = file->first_cluster;
    for (uint32_t i = 0; i < cluster_index && cluster != FAT32_CLUSTER_END; i++) {
        cluster = fat32_get_next_cluster(fs, cluster);
    }

    while (bytes_written < size && cluster >= 2 &&
           cluster != FAT32_CLUSTER_END && cluster != FAT32_CLUSTER_BAD) {
        uint32_t offset_in_cluster = file->file_position % bpc;
        uint32_t remain = size - bytes_written;

        /* Partial cluster: read-modify-write one cluster. */
        if (offset_in_cluster != 0 || remain < bpc) {
            uint32_t space_in_cluster = bpc - offset_in_cluster;
            uint32_t to_write = remain;
            if (to_write > space_in_cluster)
                to_write = space_in_cluster;
            /* Written from offset 0 with everything after the data past EOF:
             * nothing in the cluster to keep, so skip the read (on a new file
             * that was one device read per file). */
            if (offset_in_cluster == 0 &&
                (uint64_t)file->file_position + to_write >= file->file_size)
                memset(cluster_buf, 0, bpc);
            else if (fat32_read_cluster(fs, cluster, cluster_buf) != 0)
                memset(cluster_buf, 0, bpc);
            memmove((uint8_t *)cluster_buf + offset_in_cluster, src + bytes_written, to_write);
            if (fat32_write_cluster(fs, cluster, cluster_buf) != 0) {
                printf("fat32: Failed to write cluster %u\n", cluster);
                kfree(cluster_buf);
                FS_UNLOCK(); return bytes_written > 0 ? (int)bytes_written : -1;
            }
            bytes_written += to_write;
            file->file_position += to_write;
            if (offset_in_cluster + to_write >= bpc)
                cluster = fat32_get_next_cluster(fs, cluster);
            continue;
        }

        /* Full clusters that sit on consecutive LBAs go out as one DMA write. */
        uint32_t spc = fs->sectors_per_cluster ? fs->sectors_per_cluster : 1;
        uint32_t max_run = FAT32_WRITE_IO_SECTORS / spc;
        if (max_run < 1)
            max_run = 1;
        uint32_t start_sector = fat32_cluster_to_sector(fs, cluster);
        if (start_sector == 0) {
            kfree(cluster_buf);
            FS_UNLOCK(); return bytes_written > 0 ? (int)bytes_written : -1;
        }
        uint32_t run = 1;
        uint32_t tail = cluster;
        /* The last, partly filled cluster may join the run (zero padded) when
         * nothing after the data in it is inside the file. */
        bool tail_past_eof = (uint64_t)file->file_position + remain >= file->file_size;
        while (run < max_run && (uint64_t)run * bpc < (uint64_t)remain &&
               ((uint64_t)(run + 1) * bpc <= (uint64_t)remain || tail_past_eof)) {
            uint32_t next = fat32_get_next_cluster(fs, tail);
            if (next < 2 || next == FAT32_CLUSTER_END || next == FAT32_CLUSTER_BAD ||
                next == FAT32_CLUSTER_FREE)
                break;
            if (fat32_cluster_to_sector(fs, next) != start_sector + run * spc)
                break;
            tail = next;
            run++;
        }
        uint32_t run_bytes = run * bpc;
        if (run_bytes > remain)
            run_bytes = remain;      /* zero-padded partial tail cluster */
        if (run == 1) {
            memmove(cluster_buf, src + bytes_written, bpc);
            if (fat32_write_cluster(fs, cluster, cluster_buf) != 0) {
                printf("fat32: Failed to write cluster %u\n", cluster);
                kfree(cluster_buf);
                FS_UNLOCK(); return bytes_written > 0 ? (int)bytes_written : -1;
            }
        } else {
            if (!fat32_write_io_buf) {
                fat32_write_io_buf = kalloc(FAT32_WRITE_IO_SECTORS * 512u);
                if (!fat32_write_io_buf) {
                    run = 1;
                    tail = cluster;
                    run_bytes = bpc;
                    memmove(cluster_buf, src + bytes_written, bpc);
                    if (fat32_write_cluster(fs, cluster, cluster_buf) != 0) {
                        kfree(cluster_buf);
                        FS_UNLOCK(); return bytes_written > 0 ? (int)bytes_written : -1;
                    }
                    bytes_written += run_bytes;
                    file->file_position += run_bytes;
                    cluster = fat32_get_next_cluster(fs, tail);
                    continue;
                }
            }
            memcpy(fat32_write_io_buf, src + bytes_written, run_bytes);
            if (run_bytes < run * bpc)
                memset((uint8_t *)fat32_write_io_buf + run_bytes, 0, run * bpc - run_bytes);
            if (storage_write_sectors(fs->storage_dev->device_id, start_sector,
                                      run * spc, fat32_write_io_buf) != 0) {
                printf("fat32: multi-sector write failed at cluster %u (%u clusters)\n",
                       cluster, run);
                kfree(cluster_buf);
                FS_UNLOCK(); return bytes_written > 0 ? (int)bytes_written : -1;
            }
        }
        bytes_written += run_bytes;
        file->file_position += run_bytes;
        if (run_bytes % bpc == 0)
            cluster = fat32_get_next_cluster(fs, tail);
        else
            cluster = tail;          /* position is inside the tail cluster */
    }

    kfree(cluster_buf);

    // Update file size if we wrote past the old end
    if (file->file_position > file->file_size) {
        file->file_size = file->file_position;
    }

    // Update the directory entry on disk (use parent_dir_cluster for subdirs)
    uint32_t dir_start = file->parent_dir_cluster ? file->parent_dir_cluster
                                                   : fs->root_dir_cluster;
    fat32_update_dir_entry(file, dir_start);

    // Keep current_cluster in sync
    file->current_cluster = cluster;

    SERIAL_DEBUG_PRINT(FAT32, "[fat32_write] wrote %d bytes, new size=%d\n",
                       (int)bytes_written, (int)file->file_size);
    FS_UNLOCK(); return (int)bytes_written;
}

int fat32_write(fat32_file_t *file, const void *buffer, uint32_t size) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_write_impl(file, buffer, size);
    kprof_end(KP_FAT_WRITE, kp_t0, 0);
    return kp_r;
}

int fat32_seek(fat32_file_t *file, uint32_t position) {
    FS_LOCK();
    if (!file || !file->in_use) {
        FS_UNLOCK(); return -1;
    }

    // Clamp position to file size for reads (writes may extend beyond)
    if (position > file->file_size) {
        position = file->file_size;
    }

    if (file->first_cluster == 0 || file->first_cluster == FAT32_CLUSTER_END) {
        file->file_position = position;
        file->current_cluster = FAT32_CLUSTER_END;
        FS_UNLOCK(); return 0;
    }

    uint32_t bytes_per_cluster = file->filesystem->bytes_per_cluster;
    uint32_t target_cluster_index = position / bytes_per_cluster;

    /* Walk forward from the cached current cluster when possible instead of
     * re-walking the whole chain from the head: sequential seeks (the common
     * pattern) become O(1) instead of O(n). Backward seeks restart from the
     * chain head. */
    uint32_t old_cluster_index = file->file_position / bytes_per_cluster;
    uint32_t cluster, steps;
    if (file->current_cluster >= 2 &&
        file->current_cluster != FAT32_CLUSTER_END &&
        target_cluster_index >= old_cluster_index) {
        cluster = file->current_cluster;
        steps = target_cluster_index - old_cluster_index;
    } else {
        cluster = file->first_cluster;
        steps = target_cluster_index;
    }

    uint32_t walked = 0;
    for (uint32_t i = 0; i < steps; i++) {
        uint32_t next = fat32_get_next_cluster(file->filesystem, cluster);
        if (next == FAT32_CLUSTER_END || next == FAT32_CLUSTER_BAD ||
            next == FAT32_CLUSTER_FREE) {
            // Position is beyond the cluster chain; stop at last valid cluster
            break;
        }
        cluster = next;
        walked++;
    }
    /* The chain ended before a cluster the file size says must exist: the
     * FAT lookup failed or the chain is short. Report it instead of leaving
     * the file positioned on the wrong cluster (reads would return the
     * wrong data with a full byte count). */
    if (walked < steps &&
        target_cluster_index < (file->file_size + bytes_per_cluster - 1) / bytes_per_cluster) {
        serial_printf("fat32: seek to %u in '%s' failed: cluster chain ends early\n",
                      position, file->filename);
        FS_UNLOCK(); return -1;
    }

    file->file_position = position;
    file->current_cluster = cluster;
    FS_UNLOCK(); return 0;
}

static void fat32_mark_fat_dirty(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t bps = fs->boot_sector.bytes_per_sector;
    if (bps == 0) return;
    uint32_t sector = (cluster * 4) / bps;
    if (sector < fat_dirty_lo) fat_dirty_lo = sector;
    if (sector > fat_dirty_hi) fat_dirty_hi = sector;
}

/* Flush the dirty FAT sectors. ext_flags bit 7 clear means the copies are
 * mirrors, so write every copy or a reader of FAT 2 sees an empty volume. */
static int fat32_flush_fat_impl(fat32_fs_t *fs) {
    if (!fs || !fat_cache_live || !fs->is_mounted) {
        return -1;
    }
    int rc = 0;
    if (fat_dirty_lo <= fat_dirty_hi) {
        /* Dirty slots sorted by FAT sector; runs of adjacent sectors go out as
         * one multi-sector write per FAT copy instead of one command each. */
        int idx[FAT32_FAT_CACHE_SLOTS];
        int n = 0;
        for (int i = 0; i < FAT32_FAT_CACHE_SLOTS; i++) {
            if (fat_slots[i].dirty && fat_slots[i].sector != 0xFFFFFFFFu)
                idx[n++] = i;
        }
        for (int i = 1; i < n; i++) {
            int v = idx[i], j = i;
            while (j > 0 && fat_slots[idx[j - 1]].sector > fat_slots[v].sector) {
                idx[j] = idx[j - 1];
                j--;
            }
            idx[j] = v;
        }
        uint8_t *run_buf = n > 1 ? (uint8_t *)kalloc(FAT32_FAT_CACHE_SLOTS * 512u) : NULL;
        int a = 0;
        while (a < n) {
            int b = a + 1;
            if (run_buf) {
                while (b < n && fat_slots[idx[b]].sector == fat_slots[idx[b - 1]].sector + 1)
                    b++;
            }
            if (b - a == 1) {
                if (fat32_fat_slot_write(fs, &fat_slots[idx[a]]) != 0)
                    rc = -1;
            } else {
                for (int k = a; k < b; k++)
                    memcpy(run_buf + (uint32_t)(k - a) * 512u, fat_slots[idx[k]].data, 512);
                if (fat32_fat_write_run(fs, fat_slots[idx[a]].sector, (uint32_t)(b - a),
                                        run_buf) == 0) {
                    for (int k = a; k < b; k++)
                        fat_slots[idx[k]].dirty = 0;
                } else {
                    rc = -1;
                }
            }
            a = b;
        }
        if (run_buf)
            kfree(run_buf);
    }
    fat_dirty_lo = 0xFFFFFFFF;
    fat_dirty_hi = 0;
    return rc;
}

static int fat32_flush_fat(fat32_fs_t *fs) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_flush_fat_impl(fs);
    kprof_end(KP_FAT_FATFLUSH, kp_t0, 0);
    return kp_r;
}

/* FSInfo: record the allocation cursor as the next-free hint and mark the
 * free count unknown (this driver does not track it, and the value written
 * at format time is stale once files exist). Only after this mount changed
 * the FAT. Caller holds the FS lock. */
static int fat32_write_fsinfo_locked(fat32_fs_t *fs) {
    if (!fat_meta_changed || !fs || !fs->storage_dev)
        return 0;
    uint16_t fsi = fs->boot_sector.fs_info;
    uint16_t rsv = fs->boot_sector.reserved_sector_count;
    if (fsi == 0 || fsi == 0xFFFF || fsi >= rsv)
        return 0;
    uint32_t lba = fs->fat_start_sector - rsv + fsi;
    uint8_t buf[512];
    if (storage_read_sectors(fs->storage_dev->device_id, lba, 1, buf) != 0)
        return -1;
    fat32_fsinfo_t *fi = (fat32_fsinfo_t *)buf;
    if (fi->lead_signature != 0x41615252u || fi->struct_signature != 0x61417272u)
        return 0;
    if (fi->next_free == fat_alloc_cursor && fi->free_count == 0xFFFFFFFFu)
        return 0;
    fi->next_free = fat_alloc_cursor;
    fi->free_count = 0xFFFFFFFFu;
    return storage_write_sectors(fs->storage_dev->device_id, lba, 1, buf);
}

static int fat32_sync_locked(void) {
    if (!fat_cache_live || !fat32_filesystem.is_mounted)
        return 0;
    int rc = fat32_flush_fat(&fat32_filesystem);
    if (fat32_write_fsinfo_locked(&fat32_filesystem) != 0)
        rc = -1;
    return rc;
}

int fat32_sync(void) {
    FS_LOCK();
    int rc = fat32_sync_locked();
    FS_UNLOCK();
    return rc;
}

void fat32_set_deferred_flush(bool defer) {
    FS_LOCK();
    fat_defer_flush = defer;
    if (!defer && fat_cache_live && fat32_filesystem.is_mounted)
        fat32_flush_fat(&fat32_filesystem);
    FS_UNLOCK();
}

// Helper: allocate a single free cluster and mark it as end-of-chain.
// NOTE: does NOT flush the FAT — callers batch allocations and flush once.
static uint32_t fat32_allocate_cluster(fat32_fs_t *fs) {
    if (!fs || !fat_cache_live) {
        return FAT32_CLUSTER_END;
    }

    uint32_t bps = fs->boot_sector.bytes_per_sector ? fs->boot_sector.bytes_per_sector : 512;
    uint32_t total_clusters = (uint32_t)(((uint64_t)fs->boot_sector.fat_size_32 * bps) / 4);
    /* The FAT has room for more entries than the volume has data clusters
     * (the FAT is rounded up to whole sectors).  Bounding by FAT capacity
     * handed out clusters past the end of the disk: on the 100 MiB image
     * clusters 201618..201727 mapped beyond the last sector, so a write that
     * reached them failed ("IDE: Write beyond drive capacity") and returned
     * -1 at 2 MiB in ostest fs.large_24mb.  Valid data clusters are
     * 2 .. data_clusters+1. */
    {
        uint32_t tot = fs->boot_sector.total_sectors_32 ? fs->boot_sector.total_sectors_32
                                                        : fs->boot_sector.total_sectors_16;
        uint32_t spc = fs->boot_sector.sectors_per_cluster ? fs->boot_sector.sectors_per_cluster : 1;
        uint64_t meta = (uint64_t)fs->boot_sector.reserved_sector_count +
                        (uint64_t)fs->boot_sector.num_fats * fs->boot_sector.fat_size_32;
        if (tot > meta) {
            uint64_t end = (tot - meta) / spc + 2;
            if (end < total_clusters) total_clusters = (uint32_t)end;
        }
    }
    if (total_clusters <= 2) return FAT32_CLUSTER_END;
    if (fat_alloc_cursor < 2 || fat_alloc_cursor >= total_clusters)
        fat_alloc_cursor = 2;

    // Next-fit: search from the cursor, wrapping once. Each step hits the
    // FAT window cache, so a run of free clusters is one sector read, not a
    // walk of the whole table from disk.
    uint32_t start = fat_alloc_cursor;
    uint32_t cluster = start;
    do {
        uint32_t raw = 0;
        if (fat32_fat_load(fs, cluster, &raw) != 0)
            return FAT32_CLUSTER_END;
        if ((raw & 0x0FFFFFFF) == FAT32_CLUSTER_FREE) {
            if (fat32_fat_store(fs, cluster, FAT32_CLUSTER_END) != 0)
                return FAT32_CLUSTER_END;
            kprof_add(KP_FAT_ALLOC, 1);
            fat_alloc_cursor = cluster + 1;
            if (fat_alloc_cursor >= total_clusters) fat_alloc_cursor = 2;
            return cluster;
        }
        cluster++;
        if (cluster >= total_clusters) cluster = 2;
    } while (cluster != start);

    return FAT32_CLUSTER_END; // No space
}

// Helper: free an entire cluster chain (recursively clears FAT entries)
static void fat32_free_cluster_chain(fat32_fs_t *fs, uint32_t start_cluster) {
    if (!fs || !fat_cache_live || start_cluster < 2 || start_cluster == FAT32_CLUSTER_END) {
        return;
    }

    uint32_t cur = start_cluster;
    uint32_t guard = 0;
    while (cur != FAT32_CLUSTER_END && cur >= 2 &&
           guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        uint32_t next = fat32_get_next_cluster(fs, cur);
        if (fat32_fat_store(fs, cur, FAT32_CLUSTER_FREE) != 0)
            break;
        if (next == FAT32_CLUSTER_END || next == FAT32_CLUSTER_BAD || next == FAT32_CLUSTER_FREE || next < 2) {
            break;
        }
        cur = next;
    }
    fat32_flush_fat(fs);
}

/*
 * Truncate a file to zero length: release its cluster chain and clear the
 * size/cluster fields in its directory entry.  Long-name entries are left
 * untouched, so a truncated long-named file keeps its name.
 *
 * Implemented on top of the normal open path (which finds the entry and fills
 * in attributes/parent cluster) - hence the lock is taken only after
 * fat32_open() has returned.  Directories are refused: freeing a directory's
 * chain would destroy its contents.
 */
static int fat32_truncate_file_impl(const char *filename) {
    if (!fat32_is_mounted()) {
        return -1;
    }

    fat32_file_t *f = fat32_open(filename, "w");
    if (!f) {
        return -1;
    }
    if (f->attributes & FAT32_ATTR_DIRECTORY) {
        fat32_close(f);
        return -2;   /* is a directory */
    }

    if (f->first_cluster < 2 && f->file_size == 0) {
        fat32_close(f);
        return 0;   /* already empty: no chain to free, entry unchanged */
    }
    FS_LOCK();
    if (f->first_cluster >= 2) {
        fat32_free_cluster_chain(&fat32_filesystem, f->first_cluster);
    }
    f->first_cluster    = 0;
    f->current_cluster  = FAT32_CLUSTER_END;
    f->file_size        = 0;
    f->file_position    = 0;

    uint32_t dir_start = f->parent_dir_cluster ? f->parent_dir_cluster
                                               : fat32_filesystem.root_dir_cluster;
    int rc = fat32_update_dir_entry(f, dir_start);
    FS_UNLOCK();

    fat32_close(f);
    return rc;
}

int fat32_truncate_file(const char *filename) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_truncate_file_impl(filename);
    kprof_end(KP_FAT_TRUNC, kp_t0, 0);
    return kp_r;
}

/*
 * Diagnostic used when a create fails with "directory full": summarises the
 * parent directory's cluster chain and entry population so the failure can be
 * told apart from a corrupted chain or a genuinely full directory.
 */
static void fat32_dump_dir_summary(const char *tag, uint32_t cluster) {
    if (cluster == FAT32_CLUSTER_END) {
        printf("fat32[%s]: parent cluster is END – path had no parent dir\n", tag);
        return;
    }
    void *buf = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!buf) {
        printf("fat32[%s]: cannot allocate dump buffer\n", tag);
        return;
    }
    uint32_t per = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);
    uint32_t total = 0, free_used = 0, terminator = 0, deleted = 0;
    uint32_t lfn = 0, dirs = 0, files = 0, steps = 0;
    uint32_t c = cluster;
    while (c != FAT32_CLUSTER_END && c >= 2 && steps++ < 64) {
        if (fat32_read_cluster(&fat32_filesystem, c, buf) != 0) {
            printf("fat32[%s]: read of cluster %u FAILED after %u cluster(s)\n",
                   tag, c, steps - 1);
            break;
        }
        fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
        for (uint32_t i = 0; i < per; i++) {
            total++;
            uint8_t n0 = (uint8_t)e[i].name[0];
            if (n0 == 0x00) { terminator++; continue; }
            if (n0 == 0xE5) { deleted++; continue; }
            if (e[i].attr == FAT32_ATTR_LFN) { lfn++; continue; }
            if (e[i].attr & FAT32_ATTR_DIRECTORY) { dirs++; continue; }
            files++;
            free_used++;  /* keeps a count of real (non-LFN) entries */
        }
        uint32_t next = fat32_get_next_cluster(&fat32_filesystem, c);
        printf("fat32[%s]: chain cluster %u -> next 0x%X\n", tag, c, next);
        c = next;
    }
    printf("fat32[%s]: root=%u bpc=%u per-cluster=%u scanned=%u lfn=%u files=%u "
           "dirs=%u deleted=%u terminator=%u clusters=%u\n",
           tag, fat32_filesystem.root_dir_cluster,
           fat32_filesystem.bytes_per_cluster, per, total, lfn, files, dirs,
           deleted, terminator, steps);
    (void)free_used;
    kfree(buf);
}

/*
 * Find a free slot in the directory chain starting at `dir_cluster`, growing
 * the directory by one cluster when every existing slot is in use.
 *
 * FAT32 directories are ordinary cluster chains: when the last cluster has no
 * 0x00/0xE5 slot left, a new cluster has to be allocated, linked and formatted
 * before any entry can be added.  Without this the root directory (512-byte
 * clusters here – 16 entries each) silently becomes read-only once filled.
 *
 * On success `buf` holds the cluster that owns the slot, *out_slot_cluster is
 * that cluster's number and *out_slot points at the free entry inside `buf`.
 * Returns 0 on success, -1 on I/O error, -2 when the volume is out of space.
 */
static int fat32_dir_find_free_slots(fat32_fs_t *fs, uint32_t dir_cluster,
                                     void *buf, uint32_t needed,
                                     uint32_t *out_slot_cluster,
                                     uint32_t *out_slot_index) {
    if (!fs || !buf || needed == 0 || dir_cluster < 2 ||
        dir_cluster == FAT32_CLUSTER_END) {
        return -1;
    }

    uint32_t epc = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);
    if (needed > epc)
        return -3;  /* caller should drop LFN and retry with 8.3 only */

    uint32_t c = dir_cluster;
    uint32_t last = dir_cluster;
    uint32_t guard = 0;

    while (c >= 2 && c != FAT32_CLUSTER_END &&
           guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        if (fat32_read_cluster(fs, c, buf) != 0) {
            return -1;
        }
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
        uint32_t run = 0;
        uint32_t run_start = 0;
        bool hit_end_marker = false;
        for (uint32_t i = 0; i < epc; i++) {
            uint8_t n0 = (uint8_t)entries[i].name[0];
            if (n0 == 0x00 || n0 == 0xE5) {
                if (run == 0) run_start = i;
                run++;
                if (run >= needed) {
                    *out_slot_cluster = c;
                    *out_slot_index = run_start;
                    return 0;
                }
                if (n0 == 0x00) {
                    /* Directory ends at the first 0x00. Do not search later
                     * clusters for free slots. Grow instead if this cluster
                     * cannot hold `needed` contiguous slots. */
                    hit_end_marker = true;
                    run += (epc - i - 1);
                    if (run >= needed) {
                        *out_slot_cluster = c;
                        *out_slot_index = run_start;
                        return 0;
                    }
                    break;
                }
            } else {
                run = 0;
            }
        }
        last = c;
        if (hit_end_marker)
            break;
        uint32_t next = fat32_get_next_cluster(fs, c);
        if (next == FAT32_CLUSTER_END || next == FAT32_CLUSTER_BAD ||
            next < 2) {
            break;
        }
        c = next;
    }

    uint32_t new_cluster = fat32_allocate_cluster(fs);
    if (new_cluster == FAT32_CLUSTER_END) {
        return -2;
    }

    memset(buf, 0, fs->bytes_per_cluster);
    if (fat32_write_cluster(fs, new_cluster, buf) != 0) {
        return -1;
    }

    {
        uint32_t raw = 0;
        if (fat32_fat_load(fs, last, &raw) != 0)
            return -1;
        if (fat32_fat_store(fs, last, (raw & 0xF0000000u) | new_cluster) != 0)
            return -1;
    }
    /* Scrub trailing 0x00 end-markers in `last` to 0xE5 so lookups that still
     * stop at 0x00 will not ignore the newly linked cluster. */
    if (fat32_read_cluster(fs, last, buf) == 0) {
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
        bool scrubbed = false;
        for (uint32_t i = 0; i < epc; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) {
                entries[i].name[0] = (char)0xE5;
                scrubbed = true;
            }
        }
        if (scrubbed)
            (void)fat32_write_cluster(fs, last, buf);
    }
    if (fat32_flush_fat(fs) != 0) {
        return -1;
    }

    /* The caller fills its entries into `buf` and writes it to the slot
     * cluster. `buf` was just reused to scrub `last`, so without this the
     * new cluster became a copy of the previous one (every entry
     * duplicated, the cluster full again, a new cluster for each name). */
    memset(buf, 0, fs->bytes_per_cluster);
    serial_printf("fat32: Directory grew - cluster %u now continues into %u\n",
                  last, new_cluster);
    *out_slot_cluster = new_cluster;
    *out_slot_index = 0;
    return 0;
}

/* Write LFN entries (if needed) plus the 8.3 entry into a parent directory.
 * 'orig_name' is the leaf with original case. Returns 0 or -1. */
static int fat32_dir_place_named_entry(fat32_fs_t *fs, uint32_t parent_cluster,
                                       const char *orig_name, uint8_t attr,
                                       uint32_t first_cluster, uint32_t file_size) {
    char short_name[11];
    fat32_lfn_entry_t lfn_entries[20];
    int lfn_count = 0;
    uint8_t case_flags = 0;

    if (fat32_83_case_flags(orig_name, &case_flags)) {
        fat32_parse_83_filename(orig_name, short_name);
    } else if (fat32_needs_lfn(orig_name)) {
        fat32_make_short_alias(fs, parent_cluster, orig_name, short_name);
        lfn_count = fat32_lfn_build(orig_name, short_name, lfn_entries);
        if (lfn_count < 0)
            lfn_count = 0;
        /* If LFN construction failed, do NOT keep the '~1' alias alone:
         * find_in_dir(basename) looks for the uppercased leaf (e.g. TMPBB)
         * and would never match TMPBB~1 without LFN entries. */
        if (lfn_count == 0)
            fat32_parse_83_filename(orig_name, short_name);
    } else {
        fat32_parse_83_filename(orig_name, short_name);
    }

    void *cluster_buffer = kalloc(fs->bytes_per_cluster);
    if (!cluster_buffer)
        return -1;

    uint32_t needed = (uint32_t)lfn_count + 1;
    uint32_t slot_cluster = 0;
    uint32_t slot_index = 0;
    int slot_rc = fat32_dir_find_free_slots(fs, parent_cluster, cluster_buffer,
                                            needed, &slot_cluster, &slot_index);
    if (slot_rc == -3) {
        /* Name needs more slots than one cluster holds: fall back to pure 8.3. */
        lfn_count = 0;
        case_flags = 0;
        fat32_parse_83_filename(orig_name, short_name);
        needed = 1;
        slot_rc = fat32_dir_find_free_slots(fs, parent_cluster, cluster_buffer,
                                            needed, &slot_cluster, &slot_index);
    }
    if (slot_rc != 0) {
        kfree(cluster_buffer);
        fat32_dump_dir_summary("place_entry", parent_cluster);
        printf("fat32: No free directory slot for '%s' (%s)\n", orig_name,
               slot_rc == -2 ? "volume full" : "I/O error");
        return -1;
    }

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
    for (int i = 0; i < lfn_count; i++) {
        memcpy(&entries[slot_index + (uint32_t)i], &lfn_entries[i],
               sizeof(fat32_lfn_entry_t));
    }

    fat32_dir_entry_t *entry = &entries[slot_index + (uint32_t)lfn_count];
    memset(entry, 0, sizeof(*entry));
    memmove(entry->name, short_name, 11);
    entry->attr = attr;
    entry->nt_reserved = case_flags;
    entry->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    entry->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    entry->file_size = file_size;

    if (fat32_write_cluster(fs, slot_cluster, cluster_buffer) != 0) {
        kfree(cluster_buffer);
        printf("fat32: Failed to write directory entry for '%s'\n", orig_name);
        return -1;
    }
    kfree(cluster_buffer);
    fat_last_place_cluster = slot_cluster;
    fat_last_place_index = slot_index + (uint32_t)lfn_count;
    memcpy(fat_last_place_name, short_name, 11);
    return 0;
}

/* Boot CD (or any optical medium): nothing on it can be changed. */
int fat32_medium_read_only(void) {
    return fat32_filesystem.storage_dev && fat32_filesystem.storage_dev->optical;
}

static int fat32_create_file_impl(const char *filename) {
    FS_LOCK();    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        FS_UNLOCK(); return -1;
    }

    // Resolve path to parent directory + basename
    uint32_t parent_cluster = 0;
    char basename[256];
    if (fat32_resolve_path(filename, &parent_cluster, basename, sizeof(basename)) != 0) {
        printf("fat32: Cannot resolve path for '%s'\n", filename);
        FS_UNLOCK(); return -1;
    }
    if (basename[0] == '\0') {
        printf("fat32: No filename provided\n");
        FS_UNLOCK(); return -1;
    }

    {
        uint32_t dummy_cluster = 0;
        uint8_t dummy_attr = 0;
        if (fat32_find_in_dir(&fat32_filesystem, parent_cluster, basename,
                              &dummy_cluster, &dummy_attr) == 0) {
            FS_UNLOCK(); return 0;
        }
    }

    if (fat32_medium_read_only()) {
        printf("fat32: read-only medium, cannot create '%s'\n", filename);
        FS_UNLOCK(); return -30;   /* -EROFS */
    }
    if (basename[0] == '\0') {
        FS_UNLOCK(); return -1;
    }
    char leaf[256];
    fat32_path_leaf(filename, leaf, sizeof(leaf));
    if (leaf[0] == '\0')
        memcpy(leaf, basename, strlen(basename) + 1);
    if (fat32_dir_place_named_entry(&fat32_filesystem, parent_cluster, leaf,
                                    FAT32_ATTR_ARCHIVE, 0, 0) != 0) {
        printf("fat32: create: failed to place file entry for '%s'\n", filename);
        FS_UNLOCK(); return -1;
    }
    {
        size_t pl = strlen(filename);
        if (pl < sizeof(fat_create_hint.path)) {
            memcpy(fat_create_hint.path, filename, pl + 1);
            fat_create_hint.parent = parent_cluster;
            fat_create_hint.cluster = fat_last_place_cluster;
            fat_create_hint.index = fat_last_place_index;
            memcpy(fat_create_hint.name, fat_last_place_name, 11);
            fat_create_hint.valid = true;
        } else {
            fat_create_hint.valid = false;
        }
    }
    /* Serial only: a console line costs a framebuffer scroll, which was a
     * large share of the installer's per-file time. */
    serial_printf("fat32: Created empty file '%s'\n", filename);
    FS_UNLOCK(); return 0;
}

int fat32_create_file(const char *filename) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_create_file_impl(filename);
    kprof_end(KP_FAT_CREATE, kp_t0, 0);
    return kp_r;
}

static int fat32_create_directory_impl(const char *dirname) {
    FS_LOCK();
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        FS_UNLOCK(); return -1;
    }

    // Resolve path
    uint32_t parent_cluster = 0;
    char basename[256];
    if (fat32_resolve_path(dirname, &parent_cluster, basename, sizeof(basename)) != 0) {
        printf("fat32: Cannot resolve path for directory '%s'\n", dirname);
        FS_UNLOCK(); return -1;
    }
    if (basename[0] == '\0') {
        printf("fat32: No directory name provided\n");
        FS_UNLOCK(); return -1;
    }

    {
        uint32_t dummy_cluster = 0;
        uint8_t dummy_attr = 0;
        if (fat32_find_in_dir(&fat32_filesystem, parent_cluster, basename,
                              &dummy_cluster, &dummy_attr) == 0) {
            if (dummy_attr & FAT32_ATTR_DIRECTORY) {
                FS_UNLOCK(); return 0; /* idempotent mkdir */
            }
            FS_UNLOCK(); return -1; /* name taken by a file */
        }
    }

    if (fat32_medium_read_only()) {
        printf("fat32: read-only medium, cannot create directory '%s'\n", dirname);
        FS_UNLOCK(); return -30;   /* -EROFS */
    }
    uint32_t new_cluster = fat32_allocate_cluster(&fat32_filesystem);
    if (new_cluster == FAT32_CLUSTER_END) {
        printf("fat32: No space to allocate directory cluster\n");
        FS_UNLOCK(); return -28;   /* -ENOSPC */
    }

    void *new_dir_buf = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!new_dir_buf) {
        FS_UNLOCK(); return -1;
    }
    memset(new_dir_buf, 0, fat32_filesystem.bytes_per_cluster);

    fat32_dir_entry_t *dot_entries = (fat32_dir_entry_t *)new_dir_buf;

    memset(dot_entries[0].name, ' ', 11);
    dot_entries[0].name[0] = '.';
    dot_entries[0].attr = FAT32_ATTR_DIRECTORY;
    dot_entries[0].first_cluster_hi = (uint16_t)(new_cluster >> 16);
    dot_entries[0].first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    memset(dot_entries[1].name, ' ', 11);
    dot_entries[1].name[0] = '.';
    dot_entries[1].name[1] = '.';
    dot_entries[1].attr = FAT32_ATTR_DIRECTORY;
    uint32_t parent_for_dotdot = parent_cluster;
    if (parent_for_dotdot == fat32_filesystem.root_dir_cluster) {
        parent_for_dotdot = 0;
    }
    dot_entries[1].first_cluster_hi = (uint16_t)(parent_for_dotdot >> 16);
    dot_entries[1].first_cluster_lo = (uint16_t)(parent_for_dotdot & 0xFFFF);

    if (fat32_write_cluster(&fat32_filesystem, new_cluster, new_dir_buf) != 0) {
        kfree(new_dir_buf);
        printf("fat32: Failed to initialise directory cluster for '%s'\n", dirname);
        FS_UNLOCK(); return -1;
    }
    kfree(new_dir_buf);

    /* Store the name with the caller's case; lookups are case-insensitive
     * (LFN and 8.3 with NT case bits), so the uppercased basename still
     * finds it below. */
    char leaf[256];
    {
        size_t dl = strlen(dirname);
        while (dl > 1 && dirname[dl - 1] == '/') dl--;
        char tmp[256];
        if (dl >= sizeof(tmp)) dl = sizeof(tmp) - 1;
        memcpy(tmp, dirname, dl);
        tmp[dl] = '\0';
        fat32_path_leaf(tmp, leaf, sizeof(leaf));
        if (leaf[0] == '\0')
            memcpy(leaf, basename, strlen(basename) + 1);
    }
    if (basename[0] == '\0' ||
        fat32_dir_place_named_entry(&fat32_filesystem, parent_cluster, leaf,
                                    FAT32_ATTR_DIRECTORY, new_cluster, 0) != 0) {
        printf("fat32: mkdir: failed to place directory entry for '%s'\n", dirname);
        FS_UNLOCK(); return -1;
    }
    fat32_flush_fat(&fat32_filesystem);
    {
        uint32_t vc = 0;
        uint8_t va = 0;
        if (fat32_find_in_dir(&fat32_filesystem, parent_cluster, basename,
                              &vc, &va) != 0 ||
            !(va & FAT32_ATTR_DIRECTORY)) {
            printf("fat32: mkdir: entry for '%s' not visible after write "
                   "(parent_cluster=%u)\n", dirname, parent_cluster);
            FS_UNLOCK(); return -1;
        }
    }
    serial_printf("fat32: Created directory '%s' (cluster %u, parent %u)\n",
           dirname, new_cluster, parent_cluster);
    FS_UNLOCK(); return 0;
}

int fat32_create_directory(const char *dirname) {
    uint64_t kp_t0 = kprof_begin();
    int kp_r = fat32_create_directory_impl(dirname);
    kprof_end(KP_FAT_MKDIR, kp_t0, 0);
    return kp_r;
}

int fat32_delete_file(const char *filename) {
    FS_LOCK();
    fat_create_hint.valid = false;
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        FS_UNLOCK(); return -1;
    }

    // Resolve path
    uint32_t parent_cluster = 0;
    char basename[256];
    if (fat32_resolve_path(filename, &parent_cluster, basename, sizeof(basename)) != 0) {
        printf("fat32: Cannot resolve path for '%s'\n", filename);
        FS_UNLOCK(); return -1;
    }
    if (basename[0] == '\0') {
        printf("fat32: No filename provided for deletion\n");
        FS_UNLOCK(); return -1;
    }

    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: delete_file – failed to allocate buffer\n");
        FS_UNLOCK(); return -1;
    }

    char lfn_buffer[MAX_FILENAME_LEN];
    bool lfn_active = false;
    uint8_t lfn_cksum = 0;
    int lfn_seq_start = -1;      /* index of the first LFN entry of the sequence */
    bool lfn_seq_valid = false;  /* its checksum matched the short entry */

    uint32_t dir_cluster = parent_cluster;
    uint32_t chain_guard = 0;
    while (dir_cluster != FAT32_CLUSTER_END &&
           chain_guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            FS_UNLOCK(); return -1;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *e = &entries[i];
            if (((uint8_t)e->name[0]) == 0x00) {
                lfn_active = false;
                lfn_seq_start = -1;
                break; /* end of this cluster; follow chain */
            }
            if (((uint8_t)e->name[0]) == 0xE5) {
                lfn_active = false;
                lfn_seq_start = -1;
                continue;
            }

            if ((e->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)e;
                if (lfn->order & 0x40) {
                    memset(lfn_buffer, 0, sizeof(lfn_buffer));
                    lfn_active = true;
                    lfn_cksum = lfn->checksum;
                    lfn_seq_start = (int)i;
                    lfn_seq_valid = false;
                }
                if (lfn_active && lfn->checksum == lfn_cksum) {
                    fat32_lfn_extract(lfn, lfn_buffer, MAX_FILENAME_LEN);
                }
                continue;
            }

            if (e->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_active = false;
                lfn_seq_start = -1;
                continue;
            }

            bool matched = false;
            if (lfn_active) {
                uint8_t actual_ck = fat32_lfn_checksum(e->name);
                if (actual_ck == lfn_cksum && lfn_buffer[0] != '\0') {
                    lfn_seq_valid = true;
                    if (fat32_name_eq_ci(lfn_buffer, basename))
                        matched = true;
                }
                lfn_active = false;
            }
            if (!matched) {
                char formatted[256];
                fat32_format_83_filename(e->name, formatted);
                if (fat32_name_eq_ci(formatted, basename))
                    matched = true;
            }

            if (matched) {
                uint32_t first_cluster = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
                if (first_cluster >= 2) {
                    fat32_free_cluster_chain(&fat32_filesystem, first_cluster);
                }
                /* Retire the file's long-name entries too, otherwise they
                 * accumulate as orphans and eventually fill the directory. */
                if (lfn_seq_valid && lfn_seq_start >= 0 &&
                    lfn_seq_start < (int)i) {
                    for (int k = lfn_seq_start; k < (int)i; k++) {
                        entries[k].name[0] = (char)0xE5;
                    }
                }
                e->name[0] = 0xE5;
                if (fat32_write_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
                    kfree(cluster_buffer);
                    FS_UNLOCK(); return -1;
                }
                kfree(cluster_buffer);
                ktrace("fat32: Deleted file '%s'\n", filename);
                FS_UNLOCK(); return 0;
            }
            lfn_seq_start = -1;
            lfn_seq_valid = false;
        }
        lfn_seq_start = -1;
        lfn_seq_valid = false;
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }

    kfree(cluster_buffer);
    printf("fat32: File '%s' not found for deletion\n", filename);
    FS_UNLOCK(); return -1;
}


/* Unlink a directory entry (and its LFN) without freeing the cluster chain.
 * Used by rename so the data clusters move with the new name. */
static int fat32_unlink_dir_entry_keep(uint32_t parent_cluster, const char *basename_uc,
                                       uint32_t *out_first_cluster, uint32_t *out_size,
                                       uint8_t *out_attr) {
    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) return -1;

    char lfn_buffer[MAX_FILENAME_LEN];
    bool lfn_active = false;
    uint8_t lfn_cksum = 0;
    int lfn_seq_start = -1;
    bool lfn_seq_valid = false;

    uint32_t dir_cluster = parent_cluster;
    uint32_t chain_guard = 0;
    while (dir_cluster != FAT32_CLUSTER_END &&
           chain_guard++ < FAT32_MAX_CHAIN_CLUSTERS) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return -1;
        }
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
        uint32_t epc = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);
        for (uint32_t i = 0; i < epc; i++) {
            fat32_dir_entry_t *e = &entries[i];
            if (((uint8_t)e->name[0]) == 0x00) {
                lfn_active = false; lfn_seq_start = -1; break;
            }
            if (((uint8_t)e->name[0]) == 0xE5) {
                lfn_active = false; lfn_seq_start = -1; continue;
            }
            if ((e->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)e;
                if (lfn->order & 0x40) {
                    memset(lfn_buffer, 0, sizeof(lfn_buffer));
                    lfn_active = true;
                    lfn_cksum = lfn->checksum;
                    lfn_seq_start = (int)i;
                    lfn_seq_valid = false;
                }
                if (lfn_active && lfn->checksum == lfn_cksum)
                    fat32_lfn_extract(lfn, lfn_buffer, MAX_FILENAME_LEN);
                continue;
            }
            if (e->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_active = false; lfn_seq_start = -1; continue;
            }
            bool matched = false;
            if (lfn_active) {
                uint8_t actual_ck = fat32_lfn_checksum(e->name);
                if (actual_ck == lfn_cksum && lfn_buffer[0] != '\0') {
                    lfn_seq_valid = true;
                    if (fat32_name_eq_ci(lfn_buffer, basename_uc)) matched = true;
                }
                lfn_active = false;
            }
            if (!matched) {
                char formatted[256];
                fat32_format_83_filename(e->name, formatted);
                if (fat32_name_eq_ci(formatted, basename_uc)) matched = true;
            }
            if (matched) {
                if (out_first_cluster)
                    *out_first_cluster = ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
                if (out_size) *out_size = e->file_size;
                if (out_attr) *out_attr = e->attr;
                if (lfn_seq_valid && lfn_seq_start >= 0) {
                    for (int j = lfn_seq_start; j < (int)i; j++)
                        entries[j].name[0] = (char)0xE5;
                }
                e->name[0] = (char)0xE5;
                if (fat32_write_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
                    kfree(cluster_buffer);
                    return -1;
                }
                kfree(cluster_buffer);
                return 0;
            }
            lfn_seq_start = -1;
            lfn_seq_valid = false;
        }
        lfn_seq_start = -1;
        lfn_seq_valid = false;
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }
    kfree(cluster_buffer);
    return -1;
}

int fat32_rename(const char *oldpath, const char *newpath) {
    FS_LOCK();
    fat_create_hint.valid = false;
    if (!fat32_is_mounted() || !oldpath || !newpath) {
        FS_UNLOCK(); return -1;
    }

    uint32_t old_parent = 0, new_parent = 0;
    char old_base[256], new_base[256];
    if (fat32_resolve_path(oldpath, &old_parent, old_base, sizeof(old_base)) != 0 ||
        old_base[0] == '\0') {
        FS_UNLOCK(); return -ENOENT;
    }
    if (fat32_resolve_path(newpath, &new_parent, new_base, sizeof(new_base)) != 0 ||
        new_base[0] == '\0') {
        FS_UNLOCK(); return -ENOENT; /* missing intermediate dir */
    }

    /* No-op rename to self */
    if (old_parent == new_parent && fat32_name_eq_ci(old_base, new_base)) {
        FS_UNLOCK(); return 0;
    }

    /* If destination exists, replace it (POSIX rename). Directories only if empty. */
    {
        uint32_t tc = 0;
        uint8_t ta = 0;
        if (fat32_find_in_dir(&fat32_filesystem, new_parent, new_base, &tc, &ta) == 0) {
            FS_UNLOCK();
            if (ta & FAT32_ATTR_DIRECTORY) {
                int er = fat32_remove_empty_directory(newpath);
                if (er != 0) return er;
            } else {
                if (fat32_delete_file(newpath) != 0) return -EIO;
            }
            FS_LOCK();
            /* Re-resolve in case parents shifted (they shouldn't). */
            if (fat32_resolve_path(oldpath, &old_parent, old_base, sizeof(old_base)) != 0 ||
                fat32_resolve_path(newpath, &new_parent, new_base, sizeof(new_base)) != 0) {
                FS_UNLOCK(); return -ENOENT;
            }
        }
    }

    uint32_t first_cluster = 0, file_size = 0;
    uint8_t attr = 0;
    if (fat32_unlink_dir_entry_keep(old_parent, old_base,
                                    &first_cluster, &file_size, &attr) != 0) {
        FS_UNLOCK(); return -ENOENT;
    }

    /* Place the caller's spelling of the leaf (resolve_path may fold case),
     * so lowercase 8.3 names keep their NT case flags / LFN. */
    const char *new_leaf = new_base;
    {
        const char *sl = newpath;
        for (const char *q = newpath; *q; q++)
            if (*q == '/') sl = q + 1;
        if (*sl && fat32_name_eq_ci(sl, new_base)) new_leaf = sl;
    }
    if (fat32_dir_place_named_entry(&fat32_filesystem, new_parent, new_leaf,
                                    attr, first_cluster, file_size) != 0) {
        /* Best-effort rollback: put the old name back. */
        (void)fat32_dir_place_named_entry(&fat32_filesystem, old_parent, old_base,
                                          attr, first_cluster, file_size);
        FS_UNLOCK(); return -EIO;
    }

    /* Cross-directory move of a directory: update ".." to the new parent. */
    if ((attr & FAT32_ATTR_DIRECTORY) && first_cluster >= 2 &&
        old_parent != new_parent) {
        void *buf = kalloc(fat32_filesystem.bytes_per_cluster);
        if (buf && fat32_read_cluster(&fat32_filesystem, first_cluster, buf) == 0) {
            fat32_dir_entry_t *dot = (fat32_dir_entry_t *)buf;
            uint32_t parent_for_dotdot = new_parent;
            if (parent_for_dotdot == fat32_filesystem.root_dir_cluster)
                parent_for_dotdot = 0;
            /* slot 1 is ".." when we created the dir */
            if (dot[1].name[0] == '.' && dot[1].name[1] == '.') {
                dot[1].first_cluster_hi = (uint16_t)(parent_for_dotdot >> 16);
                dot[1].first_cluster_lo = (uint16_t)(parent_for_dotdot & 0xFFFF);
                (void)fat32_write_cluster(&fat32_filesystem, first_cluster, buf);
            }
        }
        if (buf) kfree(buf);
    }

    fat32_flush_fat(&fat32_filesystem);
    printf("fat32: Renamed '%s' -> '%s'\n", oldpath, newpath);
    FS_UNLOCK();
    return 0;
}

int fat32_remove_empty_directory(const char *path) {
    if (!path || path[0] == '\0')
        return -1;
    fat_create_hint.valid = false;

    enum { MAX_SCAN = 256 };
    fat32_dir_list_t *entries = (fat32_dir_list_t *)kalloc(sizeof(fat32_dir_list_t) * MAX_SCAN);
    if (!entries)
        return -1;

    int n = fat32_list_directory(path, entries, MAX_SCAN);
    if (n < 0) {
        kfree(entries);
        return -1;
    }

    int real = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = entries[i].name;
        if (nm[0] == '.' && nm[1] == '\0')
            continue;
        if (nm[0] == '.' && nm[1] == '.' && nm[2] == '\0')
            continue;
        real++;
    }
    kfree(entries);

    if (real > 0)
        return -ENOTEMPTY;

    return fat32_delete_file(path);
}

// Get file information – supports full paths (e.g. "/docs/readme.txt")
int fat32_file_exists(const char *filename) {
    FS_LOCK();
    if (!fat32_is_mounted()) {
        FS_UNLOCK(); return 0;
    }

    uint32_t parent_cluster = 0;
    char basename[256];
    if (fat32_resolve_path(filename, &parent_cluster, basename, sizeof(basename)) != 0) {
        FS_UNLOCK(); return 0;  // intermediate path missing
    }
    if (basename[0] == '\0') {
        FS_UNLOCK(); return 1;  // the path itself is a directory and it exists
    }

    uint32_t dummy_cluster = 0;
    uint8_t  dummy_attr = 0;
    if (fat32_find_in_dir(&fat32_filesystem, parent_cluster, basename,
                          &dummy_cluster, &dummy_attr) == 0) {
        FS_UNLOCK(); return 1;
    }
    FS_UNLOCK(); return 0;
}

// Formatting

/* Zero a run of sectors without reading them. Chunks match IDE DMA
 * (IDE_DMA_MAX_SECTORS, 512 KiB) so format is not one sector per command. */
#define FAT32_FMT_IO_SECTORS 1024u
static int fat32_write_sectors_buf(uint8_t device_id, uint32_t lba, uint32_t count,
                                   const void *buf, uint32_t buf_sectors) {
    while (count) {
        uint32_t n = count;
        if (n > buf_sectors)
            n = buf_sectors;
        if (storage_write_sectors(device_id, lba, n, buf) != 0)
            return -1;
        lba += n;
        count -= n;
    }
    return 0;
}

/* Set by fat32_format_range(): format exactly this LBA range and leave
 * sector 0 alone (used for GPT disks, whose LBA 0 is a protective MBR). */
static uint32_t fat32_fmt_force_start;
static uint32_t fat32_fmt_force_size;

int fat32_format_range(uint8_t device_id, const char *label,
                       uint32_t start_lba, uint32_t sectors) {
    int rc;
    if (start_lba == 0 || sectors == 0) return -1;
    fat32_fmt_force_start = start_lba;
    fat32_fmt_force_size = sectors;
    rc = fat32_format(device_id, label);
    fat32_fmt_force_start = 0;
    fat32_fmt_force_size = 0;
    return rc;
}

int fat32_format(uint8_t device_id, const char *label) {
    FS_LOCK();
    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        printf("fat32: Device %d not found\n", device_id);
        FS_UNLOCK(); return -1;
    }

    // Read MBR to find partition
    // If no partition table, create one?
    // For simplicity, we'll try to use the first available partition.
    // If none, we'll make a partition starting at sector 2048 using MBR.

    uint32_t partition_start = 0;
    uint32_t partition_size = 0;
    bool found_partition = false;

    if (fat32_fmt_force_start) {
        partition_start = fat32_fmt_force_start;
        partition_size = fat32_fmt_force_size;
        printf("fat32: Formatting LBA %u (%u sectors)\n", partition_start, partition_size);
        goto fat32_fmt_have_range;
    }

    uint8_t *mbr = kalloc(SECTOR_SIZE);
    if (!mbr) { FS_UNLOCK(); return -1; }
    
    if (storage_read_sectors(device_id, 0, 1, mbr) != 0) {
        printf("fat32: Failed to read MBR\n");
        kfree(mbr);
        FS_UNLOCK(); return -1;
    }

    // Check if it's already an MBR with a valid signature
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        // Check partitions
        for (int i = 0; i < 4; i++) {
            uint8_t *entry = mbr + 446 + i * 16;
            uint8_t type = entry[4];
            uint32_t lba = *(uint32_t*)(entry + 8);
            uint32_t sectors = *(uint32_t*)(entry + 12);
            
            if (type == 0x0B || type == 0x0C) {
                // FAT32 partition
                partition_start = lba;
                partition_size = sectors;
                found_partition = true;
                printf("fat32: Found existing FAT32 partition at LBA %u (%u sectors)\n", lba, sectors);
                break;
            }
        }
    } else {
        // Initialize MBR
        memset(mbr, 0, SECTOR_SIZE);
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
    }

    if (!found_partition) {
        // Create a new partition
        // Use LBA 2048 as start (standard alignment)
        partition_start = 2048;
        if (dev->sector_count <= partition_start) {
            printf("fat32: Device too small for partition\n");
            kfree(mbr);
            FS_UNLOCK(); return -1;
        }
        partition_size = dev->sector_count - partition_start;
        
        printf("fat32: Creating new partition at LBA %u (%u sectors)\n", partition_start, partition_size);
        
        // Write MBR entry 1
        uint8_t *entry = mbr + 446;
        entry[0] = 0x80; // Bootable
        entry[4] = 0x0C; // FAT32 LBA
        *(uint32_t*)(entry + 8) = partition_start;
        *(uint32_t*)(entry + 12) = partition_size;
        
        if (storage_write_sectors(device_id, 0, 1, mbr) != 0) {
            printf("fat32: Failed to write MBR\n");
            kfree(mbr);
            FS_UNLOCK(); return -1;
        }
    }
    
    kfree(mbr);

fat32_fmt_have_range:;
    // Now format the partition
    // Parameters
    uint32_t reserved_sectors = 32;
    uint32_t fat_copies = 2;
    /* Limine (and the FAT spec, paragraph 3.5) decides FAT12/16/32 from the
     * cluster count, not from the BPB type string. Fewer than 65525 clusters
     * is treated as FAT16, and Limine then cannot read this volume.
     * Grow the cluster size only so the FAT stays within 32 MiB, while keeping the count at or above 65525 when the
     * disk is large enough. 128 sectors/cluster is 64 KiB, about 512 GiB
     * at the 32 MiB cap. */
    /* 4 KiB clusters (the usual FAT32 default for volumes from 256 MiB up)
     * unless the volume is too small to keep 65525+ clusters with them.
     * 512-byte clusters made every directory and every file eight times as
     * many clusters, FAT entries and directory reads. */
    uint32_t sectors_per_cluster = 8;
    while (sectors_per_cluster > 1 && partition_size / sectors_per_cluster < 70000u)
        sectors_per_cluster /= 2u;
    uint32_t total_clusters = 0;
    uint32_t fat_size_sectors = 0;
    const uint32_t fat_cap_sectors = (32u * 1024u * 1024u) / 512u;
    const uint32_t fat32_min_clusters = 65525u;
    if (partition_size < 64u) {
        printf("fat32: Partition too small\n");
        FS_UNLOCK(); return -1;
    }
    for (;;) {
        total_clusters = partition_size / sectors_per_cluster;
        fat_size_sectors = (total_clusters * 4u + 511u) / 512u;
        if (partition_size > reserved_sectors + fat_copies * fat_size_sectors)
            total_clusters = (partition_size - reserved_sectors - fat_copies * fat_size_sectors) / sectors_per_cluster;
        if (fat_size_sectors <= fat_cap_sectors || sectors_per_cluster >= 128u)
            break;
        sectors_per_cluster *= 2u;
    }
    if (fat_size_sectors > fat_cap_sectors || total_clusters < 16) {
        printf("fat32: Disk is too large for an in-memory FAT even with 64KB clusters.\n");
        printf("fat32: Use a volume of about 512 GiB or smaller.\n");
        FS_UNLOCK(); return -1;
    }
    if (total_clusters < fat32_min_clusters) {
        printf("fat32: Volume has only %u clusters. Limine will treat it as FAT16 and will not boot it.\n",
               total_clusters);
        printf("fat32: Use a disk of at least 64 MiB.\n");
        FS_UNLOCK(); return -1;
    }
    printf("fat32: Using %u sectors/cluster (%u clusters, %u FAT sectors)\n",
           sectors_per_cluster, total_clusters, fat_size_sectors);
    
    // Prepare Boot Sector
    fat32_boot_sector_t *bs = kalloc(SECTOR_SIZE);
    if (!bs) { FS_UNLOCK(); return -1; }
    memset(bs, 0, SECTOR_SIZE);
    
    bs->jump_boot[0] = 0xEB;
    bs->jump_boot[1] = 0x58;
    bs->jump_boot[2] = 0x90;
    memmove(bs->oem_name, "inDevOS ", 8);
    bs->bytes_per_sector = 512;
    bs->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bs->reserved_sector_count = (uint16_t)reserved_sectors;
    bs->num_fats = (uint8_t)fat_copies;
    bs->root_entry_count = 0;
    bs->total_sectors_16 = 0;
    bs->media = 0xF8;
    bs->fat_size_16 = 0;
    bs->sectors_per_track = 63;
    bs->num_heads = 255;
    bs->hidden_sectors = partition_start;
    bs->total_sectors_32 = partition_size;
    bs->fat_size_32 = fat_size_sectors;
    bs->ext_flags = 0;
    bs->fs_version = 0;
    bs->root_cluster = 2;
    bs->fs_info = 1;
    bs->backup_boot_sector = 6;
    bs->drive_number = 0x80;
    bs->boot_signature = 0x29;
    bs->volume_id = (uint32_t)(hpet_get_ticks() ^ ((uintptr_t)bs * 0x9E3779B1u));
    
    // Volume Label
    char vol_label[11];
    memset(vol_label, ' ', 11);
    if (label) {
        for (int i = 0; i < 11 && label[i]; i++) {
            char c = label[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            vol_label[i] = c;
        }
    } else {
        memmove(vol_label, "NO NAME    ", 11);
    }
    memmove(bs->volume_label, vol_label, 11);
    memmove(bs->fs_type, "FAT32   ", 8);
    
    bs->reserved_1 = 0; // End of BPB
    
    // Boot signature
    ((uint8_t*)bs)[510] = 0x55;
    ((uint8_t*)bs)[511] = 0xAA;
    
    // Write Boot Sector and the FAT32 backup copy (sector 6).
    if (storage_write_sectors(device_id, partition_start, 1, bs) != 0 ||
        storage_write_sectors(device_id, partition_start + 6, 1, bs) != 0) {
        printf("fat32: Failed to write Boot Sector\n");
        kfree(bs);
        FS_UNLOCK(); return -1;
    }
    
    // Write FSInfo
    fat32_fsinfo_t *fsinfo = (fat32_fsinfo_t*)bs; // Reuse buffer
    memset(fsinfo, 0, SECTOR_SIZE);
    fsinfo->lead_signature = 0x41615252;
    fsinfo->struct_signature = 0x61417272;
    fsinfo->free_count = total_clusters - 1; // Approximation
    fsinfo->next_free = 3;
    fsinfo->trail_signature = 0xAA550000;
    
    if (storage_write_sectors(device_id, partition_start + 1, 1, fsinfo) != 0) {
        printf("fat32: Failed to write FSInfo\n");
        kfree(bs);
        FS_UNLOCK(); return -1;
    }
    
    // Write Backup Boot Sector (sector 6)
    // We need to restore BS content to buffer
    // Re-calculating/filling BS in buffer or just re-read? easier to just fill it again or keep a copy.
    // I'll just skip backup BS logic details for brevity, but strictly it should be there.
    // Let's rely on primary for now.

    kfree(bs);

    // Initialize FAT tables. Do not read the on-disk FAT (a 32 GB volume
    // is tens of megabytes). Write the two reserved entries, then zero the
    // rest in multi-sector DMA-sized chunks.
    uint32_t fat_lba = partition_start + reserved_sectors;
    uint32_t zero_sectors = FAT32_FMT_IO_SECTORS;
    uint8_t *zero = kalloc(zero_sectors * 512u);
    if (!zero) {
        zero_sectors = 128;
        zero = kalloc(zero_sectors * 512u);
    }
    if (!zero) {
        zero_sectors = 1;
        zero = kalloc(512);
    }
    if (!zero) { FS_UNLOCK(); return -1; }
    printf("fat32: clearing FATs, %u sectors per write (%u KiB)\n",
           zero_sectors, (zero_sectors * 512u) / 1024u);

    uint32_t *fat_buf = (uint32_t *)zero;
    memset(zero, 0, 512);
    fat_buf[0] = 0x0FFFFFF8;
    fat_buf[1] = 0x0FFFFFFF;
    fat_buf[2] = 0x0FFFFFFF;
    if (storage_write_sectors(device_id, fat_lba, 1, zero) != 0) {
        kfree(zero);
        FS_UNLOCK(); return -1;
    }
    memset(zero, 0, zero_sectors * 512u);
    if (fat_size_sectors > 1 &&
        fat32_write_sectors_buf(device_id, fat_lba + 1, fat_size_sectors - 1,
                                zero, zero_sectors) != 0) {
        kfree(zero);
        FS_UNLOCK(); return -1;
    }

    uint32_t fat2_lba = fat_lba + fat_size_sectors;
    memset(zero, 0, 512);
    fat_buf[0] = 0x0FFFFFF8;
    fat_buf[1] = 0x0FFFFFFF;
    fat_buf[2] = 0x0FFFFFFF;
    if (storage_write_sectors(device_id, fat2_lba, 1, zero) != 0) {
        kfree(zero);
        FS_UNLOCK(); return -1;
    }
    memset(zero, 0, zero_sectors * 512u);
    if (fat_size_sectors > 1 &&
        fat32_write_sectors_buf(device_id, fat2_lba + 1, fat_size_sectors - 1,
                                zero, zero_sectors) != 0) {
        kfree(zero);
        FS_UNLOCK(); return -1;
    }

    uint32_t data_start = partition_start + reserved_sectors + (fat_copies * fat_size_sectors);
    memset(zero, 0, zero_sectors * 512u);
    if (sectors_per_cluster > zero_sectors ||
        fat32_write_sectors_buf(device_id, data_start, sectors_per_cluster,
                                zero, zero_sectors) != 0) {
        printf("fat32: failed to clear the root directory\n");
        kfree(zero);
        FS_UNLOCK(); return -1;
    }

    kfree(zero);

    printf("fat32: Format complete. Volume label: %s\n", label ? label : "NO NAME");
    FS_UNLOCK(); return 0;
}

/*
 * Set an open file's length (ftruncate/truncate).  Shrinking ends the
 * cluster chain after the last cluster still needed and frees the rest (or
 * the whole chain for 0); growing appends zeros through the write path.
 * The file position is kept (clamped to the new size).
 */
int fat32_truncate_handle(fat32_file_t *f, uint32_t len) {
    if (!f || !f->in_use) return -EBADF;
    if (f->attributes & FAT32_ATTR_DIRECTORY) return -EISDIR;
    uint32_t pos = f->file_position;
    if (len < f->file_size) {
        FS_LOCK();
        fat32_fs_t *fs = f->filesystem;
        uint32_t bpc = fs->bytes_per_cluster;
        uint32_t keep = (len + bpc - 1) / bpc;
        if (f->first_cluster >= 2 && f->first_cluster < FAT32_CLUSTER_BAD) {
            if (keep == 0) {
                fat32_free_cluster_chain(fs, f->first_cluster);
                f->first_cluster = 0;
            } else {
                uint32_t last = f->first_cluster;
                for (uint32_t i = 1; i < keep; i++) {
                    uint32_t nx = fat32_get_next_cluster(fs, last);
                    if (nx < 2 || nx >= FAT32_CLUSTER_BAD) break;
                    last = nx;
                }
                uint32_t next = fat32_get_next_cluster(fs, last);
                if (next >= 2 && next < FAT32_CLUSTER_BAD) {
                    if (fat32_fat_store(fs, last, FAT32_CLUSTER_END) != 0) {
                        FS_UNLOCK(); return -EIO;
                    }
                    fat32_free_cluster_chain(fs, next);
                }
            }
        }
        f->file_size       = len;
        f->file_position   = 0;
        f->current_cluster = f->first_cluster >= 2 ? f->first_cluster : FAT32_CLUSTER_END;
        uint32_t dir_start = f->parent_dir_cluster ? f->parent_dir_cluster
                                                   : fat32_filesystem.root_dir_cluster;
        int rc = fat32_update_dir_entry(f, dir_start);
        FS_UNLOCK();
        if (rc != 0) return -EIO;
    } else if (len > f->file_size) {
        uint8_t *z = (uint8_t *)kalloc(4096);
        if (!z) return -ENOMEM;
        memset(z, 0, 4096);
        int err = 0;
        if (fat32_seek(f, f->file_size) < 0) err = -EIO;
        uint32_t left = len - f->file_size;
        while (!err && left) {
            uint32_t n = left > 4096 ? 4096 : left;
            int w = fat32_write(f, z, n);
            if (w <= 0) { err = w < 0 && w != -1 ? w : -EIO; break; }
            left -= (uint32_t)w;
        }
        kfree(z);
        if (err) return err;
    }
    fat32_seek(f, pos);
    return 0;
}

int fat32_truncate_to(const char *filename, uint32_t len) {
    if (len == 0) return fat32_truncate_file(filename);
    if (!fat32_is_mounted()) return -EIO;
    fat32_file_t *f = fat32_open(filename, "w");
    if (!f) return -ENOENT;
    int r = fat32_truncate_handle(f, len);
    int c = fat32_close(f);
    return r ? r : (c ? -EIO : 0);
}

int fat32_truncate_handle_v(void *f, uint32_t len) {
    return fat32_truncate_handle((fat32_file_t *)f, len);
}
