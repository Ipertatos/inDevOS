#include "storage.h"
#include "kprof.h"
#include "limine.h"
#include "utils.h"
#include "mem.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>
#include "string.h"
#include "atomic.h"
#include "nvme.h"
#include "ktime.h"

// ============================================================================
// LRU Block Cache
// ============================================================================
// Transparent write-through cache for disk sectors.  Caches up to
// BCACHE_CAPACITY sectors keyed by (device_id, lba).  Uses a hash table
// for O(1) lookup and a doubly-linked LRU list for eviction.

/* Doom (and other apps) re-read WAD lumps via lseek+read after PU_CACHE
 * purges. 128 KB was far too small and thrashed on every miss → AHCI.
 * 8192 sectors = 4 MB covers shareware DOOM.WAD hot working set. */
#define BCACHE_CAPACITY     8192        /* max cached sectors (~4 MB) */
#define BCACHE_HASH_BUCKETS 1024        /* hash table size (power of 2) */

typedef struct bcache_entry {
    uint8_t  device_id;
    uint64_t lba;
    bool     valid;
    bool     dirty;                     /* for future write-back mode */
    uint8_t  data[SECTOR_SIZE];

    /* LRU doubly-linked list */
    struct bcache_entry *lru_prev;
    struct bcache_entry *lru_next;

    /* Hash chain */
    struct bcache_entry *hash_next;
} bcache_entry_t;

static struct {
    bcache_entry_t  entries[BCACHE_CAPACITY];
    bcache_entry_t *hash[BCACHE_HASH_BUCKETS];
    bcache_entry_t *lru_head;           /* most recently used */
    bcache_entry_t *lru_tail;           /* least recently used */
    int             count;              /* number of valid entries */
    int             free_hint;          /* rolling start index for free-slot search */
    uint64_t        hits;
    uint64_t        misses;
    bool            enabled;
} bcache;

static uint32_t bcache_hash(uint8_t dev, uint64_t lba) {
    uint64_t key = ((uint64_t)dev << 56) ^ lba;
    return (uint32_t)((key * 2654435761ULL) >> 32) & (BCACHE_HASH_BUCKETS - 1);
}

static void bcache_init(void) {
    memset(&bcache, 0, sizeof(bcache));
    bcache.enabled = true;
    serial_printf("[bcache] Block cache initialized (%u sectors, %u KB)\n",
                  (unsigned)BCACHE_CAPACITY,
                  (unsigned)(BCACHE_CAPACITY * SECTOR_SIZE / 1024));
}

/* Remove entry from the LRU list (does NOT remove from hash) */
static void bcache_lru_remove(bcache_entry_t *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else              bcache.lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else              bcache.lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

/* Push entry to the head of the LRU list (most recently used) */
static void bcache_lru_push_front(bcache_entry_t *e) {
    e->lru_prev = NULL;
    e->lru_next = bcache.lru_head;
    if (bcache.lru_head) bcache.lru_head->lru_prev = e;
    bcache.lru_head = e;
    if (!bcache.lru_tail) bcache.lru_tail = e;
}

/* Look up a cached sector.  Returns NULL on miss. */
static bcache_entry_t* bcache_lookup(uint8_t dev, uint64_t lba) {
    uint32_t h = bcache_hash(dev, lba);
    for (bcache_entry_t *e = bcache.hash[h]; e; e = e->hash_next) {
        if (e->valid && e->device_id == dev && e->lba == lba) {
            /* Move to front of LRU */
            bcache_lru_remove(e);
            bcache_lru_push_front(e);
            return e;
        }
    }
    return NULL;
}

/* Insert (or update) a sector in the cache */
static void bcache_insert(uint8_t dev, uint64_t lba, const void *data) {
    if (!bcache.enabled) return;

    /* Already cached?  Update in place. */
    bcache_entry_t *e = bcache_lookup(dev, lba);
    if (e) {
        memcpy(e->data, data, SECTOR_SIZE);
        return;
    }

    /* Need a free entry.  If cache is full, evict LRU tail. */
    if (bcache.count >= BCACHE_CAPACITY) {
        e = bcache.lru_tail;
        if (!e) return;

        /* Remove from hash chain */
        uint32_t oh = bcache_hash(e->device_id, e->lba);
        bcache_entry_t **pp = &bcache.hash[oh];
        while (*pp && *pp != e) pp = &(*pp)->hash_next;
        if (*pp) *pp = e->hash_next;

        bcache_lru_remove(e);
        e->valid = false;
        bcache.count--;
    } else {
        /* Find an unused entry starting from a rolling hint.  Scanning from
         * index 0 on every insert made filling the cache quadratic (the FAT
         * load alone inserts thousands of sectors). */
        e = NULL;
        for (int k = 0; k < BCACHE_CAPACITY; k++) {
            int idx = bcache.free_hint + k;
            if (idx >= BCACHE_CAPACITY) idx -= BCACHE_CAPACITY;
            if (!bcache.entries[idx].valid) {
                e = &bcache.entries[idx];
                bcache.free_hint = (idx + 1 >= BCACHE_CAPACITY) ? 0 : idx + 1;
                break;
            }
        }
        if (!e) return;  /* should not happen */
    }

    /* Populate entry */
    e->device_id = dev;
    e->lba       = lba;
    e->valid     = true;
    e->dirty     = false;
    memcpy(e->data, data, SECTOR_SIZE);

    /* Insert into hash chain */
    uint32_t h = bcache_hash(dev, lba);
    e->hash_next = bcache.hash[h];
    bcache.hash[h] = e;

    /* Push to LRU front */
    bcache_lru_push_front(e);
    bcache.count++;
}

/* Invalidate a range of sectors (after write-through) */
static void bcache_invalidate(uint8_t dev, uint64_t lba, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        bcache_entry_t *e = bcache_lookup(dev, lba + i);
        if (e) {
            uint32_t h = bcache_hash(dev, lba + i);
            bcache_entry_t **pp = &bcache.hash[h];
            while (*pp && *pp != e) pp = &(*pp)->hash_next;
            if (*pp) *pp = e->hash_next;
            bcache_lru_remove(e);
            e->valid = false;
            bcache.count--;
        }
    }
}

void storage_bcache_invalidate_all(void) {
    for (uint32_t i = 0; i < BCACHE_CAPACITY; i++) {
        bcache_entry_t *e = &bcache.entries[i];
        if (!e->valid) continue;
        uint32_t h = bcache_hash(e->device_id, e->lba);
        bcache_entry_t **pp = &bcache.hash[h];
        while (*pp && *pp != e) pp = &(*pp)->hash_next;
        if (*pp) *pp = e->hash_next;
        bcache_lru_remove(e);
        e->valid = false;
        e->dirty = false;
        e->hash_next = NULL;
        if (bcache.count) bcache.count--;
    }
    bcache.lru_head = bcache.lru_tail = NULL;
}

// ============================================================================

storage_device_t storage_devices[MAX_STORAGE_DEVICES];
uint8_t storage_device_count = 0;

// RAM disk simulation
#define RAMDISK_SIZE_MB 16
#define RAMDISK_SECTORS (RAMDISK_SIZE_MB * 1024 * 1024 / SECTOR_SIZE)
static uint8_t *ramdisk_data = NULL;

static spinlock_t storage_lock = {0};

/* Batched sequential reads. IDE BMIDE READ DMA EXT takes up to 1024 sectors
 * (512 KiB) in one PRDT. PIO fallback and AHCI still split inside the driver
 * (AHCI bounce is 128 sectors). */
#define STORAGE_HW_READ_MAX_SECTORS 1024
#define STORAGE_READAHEAD_SECTORS 64
static uint8_t storage_seq_hw_buf[STORAGE_HW_READ_MAX_SECTORS * SECTOR_SIZE];

// Storage operations for RAM disk
static int ramdisk_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!ramdisk_data || lba + sector_count > RAMDISK_SECTORS) {
        printf("storage: Read beyond bounds - LBA %llu + count %u > max %u\n", 
               (unsigned long long)lba, sector_count, RAMDISK_SECTORS);
        return -1;
    }
    
    uint64_t offset = lba * SECTOR_SIZE;
    uint32_t bytes_to_read = sector_count * SECTOR_SIZE;
    
    // Debug: printf("storage: Reading %u sectors from LBA %llu\n", sector_count, (unsigned long long)lba);
    
    memmove(buffer, ramdisk_data + offset, bytes_to_read);
    return 0;
}

static int ramdisk_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    if (!ramdisk_data || lba + sector_count > RAMDISK_SECTORS) {
        return -1;
    }
    
    uint64_t offset = lba * SECTOR_SIZE;
    uint32_t bytes_to_write = sector_count * SECTOR_SIZE;
    
    memmove(ramdisk_data + offset, buffer, bytes_to_write);
    return 0;
}

static int ramdisk_flush(storage_device_t *dev) {
    // Nothing to flush for RAM disk
    return 0;
}

storage_ops_t storage_ops = {
    .read_sectors = ramdisk_read_sectors,
    .write_sectors = ramdisk_write_sectors,
    .flush = ramdisk_flush
};

void storage_init() {
    DEBUG_SERIAL("storage_init: Before init - device_count=%d\n", storage_device_count);
    
    // Always ensure uninitialized entries are zeroed
    for (uint8_t i = storage_device_count; i < MAX_STORAGE_DEVICES; i++) {
        memset(&storage_devices[i], 0, sizeof(storage_device_t));
    }
    
    if (storage_device_count == 0) {
        DEBUG_SERIAL("storage_init: Clearing storage devices array\n");
    } else {
        DEBUG_SERIAL("storage_init: Preserving %d existing devices, clearing remaining slots\n", storage_device_count);
    }
    
    printf("storage: Initializing storage subsystem\n");
    bcache_init();
    DEBUG_SERIAL("storage_init: After init - device_count=%d\n", storage_device_count);
}

void storage_init_ramdisk() {
    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        printf("storage: Maximum storage devices reached\n");
        return;
    }
    
    // Allocate RAM disk
    ramdisk_data = kalloc(RAMDISK_SIZE_MB * 1024 * 1024);
    if (!ramdisk_data) {
        printf("storage: Failed to allocate RAM disk\n");
        return;
    }
    
    // Clear the RAM disk
    memset(ramdisk_data, 0, RAMDISK_SIZE_MB * 1024 * 1024);
    
    // Create simple FAT32 boot sector
    uint8_t boot_sector[SECTOR_SIZE];
    memset(boot_sector, 0, SECTOR_SIZE);
    
    // FAT32 boot sector data
    boot_sector[0] = 0xEB; // Jump instruction
    boot_sector[1] = 0x58;
    boot_sector[2] = 0x90;
    
    // OEM Name
    memmove(boot_sector + 3, "inDevOS ", 8);
    
    // Bytes per sector (512)
    *(uint16_t*)(boot_sector + 11) = 512;
    
    // Sectors per cluster (8 = 4KB clusters)
    boot_sector[13] = 8;
    
    // Reserved sectors
    *(uint16_t*)(boot_sector + 14) = 32;
    
    // Number of FATs
    boot_sector[16] = 2;
    
    // Root entries (0 for FAT32)
    *(uint16_t*)(boot_sector + 17) = 0;
    
    // Total sectors (small, set to 0)
    *(uint16_t*)(boot_sector + 19) = 0;
    
    // Media descriptor
    boot_sector[21] = 0xF8;
    
    // FAT size 16 (0 for FAT32)
    *(uint16_t*)(boot_sector + 22) = 0;
    
    // Sectors per track
    *(uint16_t*)(boot_sector + 24) = 63;
    
    // Number of heads
    *(uint16_t*)(boot_sector + 26) = 255;
    
    // Hidden sectors
    *(uint32_t*)(boot_sector + 28) = 0;
    
    // Total sectors (32-bit)
    *(uint32_t*)(boot_sector + 32) = RAMDISK_SECTORS;
    
    /* FAT size: 4 bytes per cluster. Reserved=32, SPC=8. Solve fat_sectors. */
    {
        uint32_t reserved = 32;
        uint32_t spc = 8;
        uint32_t fat_sectors = 32;
        for (int iter = 0; iter < 8; iter++) {
            uint32_t data = RAMDISK_SECTORS - reserved - 2 * fat_sectors;
            uint32_t clusters = data / spc;
            uint32_t needed = ((clusters + 2) * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
            if (needed == fat_sectors)
                break;
            fat_sectors = needed ? needed : 1;
        }
        *(uint32_t*)(boot_sector + 36) = fat_sectors;
    }
    
    // Extended flags
    *(uint16_t*)(boot_sector + 40) = 0;
    
    // Filesystem version
    *(uint16_t*)(boot_sector + 42) = 0;
    
    // Root cluster
    *(uint32_t*)(boot_sector + 44) = 2;
    
    // FS Info sector
    *(uint16_t*)(boot_sector + 48) = 1;
    
    // Backup boot sector
    *(uint16_t*)(boot_sector + 50) = 6;
    
    // Drive number
    boot_sector[64] = 0x80;
    
    // Boot signature
    boot_sector[66] = 0x29;
    
    // Volume ID
    *(uint32_t*)(boot_sector + 67) = 0x12345678;
    
    // Volume label
    memmove(boot_sector + 71, "inDevOS    ", 11);
    
    // FS type
    memmove(boot_sector + 82, "FAT32   ", 8);
    
    // Boot signature
    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;
    
    // Write boot sector to RAM disk
    memmove(ramdisk_data, boot_sector, SECTOR_SIZE);
    
    // Initialize FAT table (2 copies)
    uint32_t fat_size_sectors = 32; // Match the value we set in boot sector
    uint32_t fat_start_sector = 32; // Reserved sectors
    
    // Clear FAT area
    memset(ramdisk_data + (fat_start_sector * SECTOR_SIZE), 0, fat_size_sectors * SECTOR_SIZE * 2);
    
    // Set up FAT table entries
    uint32_t *fat_table = (uint32_t*)(ramdisk_data + (fat_start_sector * SECTOR_SIZE));
    
    // FAT[0] = media descriptor + EOC marker
    fat_table[0] = 0x0FFFFFF8; // Media descriptor (F8) + 0x0FFFFF
    // FAT[1] = clean shutdown + no errors + EOC
    fat_table[1] = 0x0FFFFFFF; // Clean shutdown + no disk errors + EOC
    // FAT[2] = root directory cluster (end of chain)
    fat_table[2] = 0x0FFFFFFF; // Root directory cluster is end-of-chain
    
    // Copy first FAT to second FAT
    memmove(ramdisk_data + ((fat_start_sector + fat_size_sectors) * SECTOR_SIZE),
            fat_table, fat_size_sectors * SECTOR_SIZE);
    
    // Initialize root directory cluster
    uint32_t data_start_sector = fat_start_sector + (2 * fat_size_sectors);
    uint32_t root_cluster_sector = data_start_sector; // Cluster 2 starts here
    uint32_t sectors_per_cluster = 8;
    
    // Clear root directory cluster (cluster 2)
    memset(ramdisk_data + (root_cluster_sector * SECTOR_SIZE), 0, 
           sectors_per_cluster * SECTOR_SIZE);
    
    printf("storage: FAT32 layout - FAT start: %d, Data start: %d, Root at: %d\n",
           fat_start_sector, data_start_sector, root_cluster_sector);
    
    // Initialize storage device
    DEBUG_SERIAL("storage_init_ramdisk: Creating ramdisk as device %d\n", storage_device_count);
    storage_device_t *dev = &storage_devices[storage_device_count];
    dev->device_id = storage_device_count;
    storage_device_count++;
    dev->sector_count = RAMDISK_SECTORS;
    dev->sector_size = SECTOR_SIZE;
    dev->is_initialized = true;
    dev->pci_dev = NULL;
    dev->base_address = (uint64_t)ramdisk_data;
    
    // Properly initialize the name field
    memset(dev->name, 0, sizeof(dev->name));
    memmove(dev->name, "RAM Disk", 8);
    
    printf("storage: RAM disk initialized (%d MB, %d sectors)\n", 
           RAMDISK_SIZE_MB, RAMDISK_SECTORS);
}

storage_device_t* storage_get_device(uint8_t device_id) {
    for (uint8_t i = 0; i < storage_device_count; i++) {
        if (storage_devices[i].device_id == device_id && storage_devices[i].is_initialized) {
            return &storage_devices[i];
        }
    }
    return NULL;
}

/* Internal: read sectors directly from hardware (no cache) */
kprof_slot_t kprof_slots[KP_COUNT];
const char *const kprof_names[KP_COUNT] = {
    "storage read (driver)", "storage read (cache only)", "storage write (driver)",
    "storage flush", "fat32 resolve_path", "fat32 find_in_dir", "fat32 dir clusters scanned",
    "fat32 create_file", "fat32 mkdir", "fat32 open", "fat32 read", "fat32 write",
    "fat32 update_dir_entry", "fat32 flush_fat", "fat32 clusters allocated",
    "fat32 truncate", "vfs exists", "vfs mkdir -p", "install: walk", "install: read files",
    "install: mkdir", "install: write files", "install: log output", "install: final flush",
};

void kprof_reset(void) {
    memset(kprof_slots, 0, sizeof(kprof_slots));
}

void kprof_dump(const char *tag) {
    for (int i = 0; i < KP_COUNT; i++) {
        kprof_slot_t *s = &kprof_slots[i];
        if (!s->count && !s->us)
            continue;
        serial_printf("kprof %s: %s n=%llu ms=%llu sectors=%llu\n", tag, kprof_names[i],
                      (unsigned long long)s->count, (unsigned long long)(s->us / 1000u),
                      (unsigned long long)s->sectors);
    }
}

static int storage_read_sectors_hw(storage_device_t *dev, uint8_t device_id,
                                   uint64_t lba, uint32_t sector_count, void *buffer) {
    (void)device_id;
    int result = -1;
    uint64_t kp_t0 = kprof_begin();
    if (dev->pci_dev == NULL) {
        result = storage_ops.read_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x01) {
        extern int ide_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
        result = ide_read_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x06) {
        extern int ahci_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
        result = ahci_read_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x08) {
        result = nvme_read_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x0C && dev->pci_dev->subclass_code == 0x03) {
        extern int usb_msc_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
        result = usb_msc_read_sectors(dev, lba, sector_count, buffer);
    }
    kprof_end(KP_ST_RD_HW, kp_t0, sector_count);
    return result;
}

int storage_read_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, void *buffer) {
    spinlock_lock(&storage_lock);
    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        spinlock_unlock(&storage_lock);
        return -1;
    }

    uint8_t *dst = (uint8_t *)buffer;
    int result = 0;
    int kp_hw = 0;
    uint64_t kp_t0 = kprof_begin();

    for (uint32_t i = 0; i < sector_count; ) {
        bcache_entry_t *hit = bcache_lookup(device_id, lba + i);
        if (hit) {
            memcpy(dst + i * SECTOR_SIZE, hit->data, SECTOR_SIZE);
            bcache.hits++;
            i++;
            continue;
        }

        bcache.misses++;
        uint32_t n = 1;
        while (i + n < sector_count && n < STORAGE_HW_READ_MAX_SECTORS) {
            if (bcache_lookup(device_id, lba + i + n))
                break;
            n++;
        }
        /* Readahead for small requests: a directory cluster, a FAT sector or
         * a small file costs one device command whatever its length (tens of
         * ms under emulation), and the next request usually wants the sectors
         * right after it. Pull up to STORAGE_READAHEAD_SECTORS into the cache.
         * DMA devices only (AHCI, NVMe): on PIO IDE every sector costs. */
        uint32_t n_req = n;
        if (i + n == sector_count && sector_count < STORAGE_READAHEAD_SECTORS &&
            bcache.enabled && dev->sector_count && dev->pci_dev &&
            ((dev->pci_dev->class_code == 0x01 &&
              (dev->pci_dev->subclass_code == 0x06 || dev->pci_dev->subclass_code == 0x08)) ||
             (dev->pci_dev->class_code == 0x0C && dev->pci_dev->subclass_code == 0x03)) &&
            (dev->sector_size == 0 || dev->sector_size == SECTOR_SIZE)) {
            while (n < STORAGE_READAHEAD_SECTORS &&
                   lba + i + n < (uint64_t)dev->sector_count &&
                   !bcache_lookup(device_id, lba + i + n))
                n++;
        }

        kp_hw = 1;
        int r = storage_read_sectors_hw(dev, device_id, lba + i, n, storage_seq_hw_buf);
        if (r != 0) {
            result = r;
            break;
        }

        for (uint32_t j = 0; j < n; j++) {
            const uint8_t *src = storage_seq_hw_buf + j * SECTOR_SIZE;
            if (j < n_req)
                memcpy(dst + (i + j) * SECTOR_SIZE, src, SECTOR_SIZE);
            bcache_insert(device_id, lba + i + j, src);
        }
        i += n_req;
    }

    if (!kp_hw)
        kprof_end(KP_ST_RD_CACHED, kp_t0, sector_count);
    spinlock_unlock(&storage_lock);
    return result;
}

int storage_write_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, const void *buffer) {
    spinlock_lock(&storage_lock);
    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        spinlock_unlock(&storage_lock);
        return -1;
    }
    
    int result = -1;
    uint64_t kp_t0 = kprof_begin();
    // Dispatch to appropriate driver based on device type
    if (dev->pci_dev == NULL) {
        result = storage_ops.write_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x01) {
        extern int ide_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
        result = ide_write_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x06) {
        extern int ahci_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
        result = ahci_write_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x08) {
        result = nvme_write_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x0C && dev->pci_dev->subclass_code == 0x03) {
        extern int usb_msc_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
        result = usb_msc_write_sectors(dev, lba, sector_count, buffer);
    }
    kprof_end(KP_ST_WR_HW, kp_t0, sector_count);

    /* Write-through for small writes. Format and file-copy runs are hundreds
     * of sectors of zeros or payload; inserting each one evicts the cache
     * and does not help the installer. Drop any cached copies instead. */
    if (result == 0 && bcache.enabled) {
        if (sector_count > 8) {
            bcache_invalidate(device_id, lba, sector_count);
        } else {
            const uint8_t *src = (const uint8_t *)buffer;
            for (uint32_t i = 0; i < sector_count; i++)
                bcache_insert(device_id, lba + i, src + i * SECTOR_SIZE);
        }
    }

    spinlock_unlock(&storage_lock);
    return result;
}

static int storage_flush_impl(uint8_t device_id) {
    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        return -1;
    }
    
    // Dispatch to appropriate driver based on device type
    if (dev->pci_dev == NULL) {
        // RAM disk
        return storage_ops.flush(dev);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x01) {
        // IDE device
        extern int ide_flush(storage_device_t *dev);
        return ide_flush(dev);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x06) {
        // SATA device
        extern int ahci_flush(storage_device_t *dev);
        return ahci_flush(dev);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x08) {
        // NVMe device
        return nvme_flush(dev);
    } else if (dev->pci_dev->class_code == 0x0C && dev->pci_dev->subclass_code == 0x03) {
        // USB mass storage (xHCI)
        extern int usb_msc_flush(storage_device_t *dev);
        return usb_msc_flush(dev);
    }
    
    return -1; // Unsupported device type
}

int storage_flush(uint8_t device_id) {
    uint64_t kp_t0 = kprof_begin();
    int r = storage_flush_impl(device_id);
    kprof_end(KP_ST_FLUSH, kp_t0, 0);
    return r;
}

/* GPT: first partition whose type is EFI System or Microsoft Basic Data and
 * whose first sector is a FAT32 boot sector. Returns 0 if none. */
static const uint8_t gpt_type_esp[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const uint8_t gpt_type_basic[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };

static int storage_is_fat32_boot(const uint8_t *s) {
    return s[510] == 0x55 && s[511] == 0xAA &&
           *(const uint16_t *)(s + 11) == 512 && s[13] != 0 &&
           *(const uint16_t *)(s + 22) == 0 && *(const uint32_t *)(s + 36) != 0;
}

static uint32_t storage_gpt_find_fat32(uint8_t device_id, uint8_t *buf) {
    if (storage_read_sectors(device_id, 1, 1, buf) != 0 || memcmp(buf, "EFI PART", 8) != 0) {
        printf("storage: protective MBR but no GPT header\n");
        return 0;
    }
    uint64_t ent_lba = *(uint64_t *)(buf + 72);
    uint32_t nents = *(uint32_t *)(buf + 80);
    uint32_t esize = *(uint32_t *)(buf + 84);
    if (esize < 128 || esize > 512 || (512 % esize) != 0 || nents > 1024 || ent_lba < 2) {
        printf("storage: GPT header has unusual entry layout (size %u, count %u)\n", esize, nents);
        return 0;
    }
    uint32_t per = 512 / esize;
    uint32_t found = 0;
    for (uint32_t i = 0; i < nents && !found; i++) {
        if (i % per == 0 && storage_read_sectors(device_id, ent_lba + i / per, 1, buf) != 0)
            return 0;
        const uint8_t *e = buf + (i % per) * esize;
        int esp = memcmp(e, gpt_type_esp, 16) == 0;
        int basic = memcmp(e, gpt_type_basic, 16) == 0;
        uint64_t first = *(const uint64_t *)(e + 32);
        if (!esp && !basic) continue;
        printf("storage: GPT partition %u %s first LBA %llu\n", i,
               esp ? "EFI System" : "Basic Data", (unsigned long long)first);
        if (first == 0 || first > 0xFFFFFFFFull) continue;
        uint8_t *bs = kalloc(512);
        if (!bs) return 0;
        if (storage_read_sectors(device_id, first, 1, bs) == 0 && storage_is_fat32_boot(bs))
            found = (uint32_t)first;
        kfree(bs);
    }
    if (found)
        printf("storage: Found FAT32 GPT partition at LBA %u\n", found);
    else
        printf("storage: No FAT32 partition in GPT\n");
    return found;
}

// Helper: Parse MBR (or GPT behind a protective MBR) and return the first FAT32 partition LBA (0 if not found)
uint32_t find_first_fat32_partition_lba(uint8_t device_id) {
    uint8_t *mbr = kalloc(512);
    if (!mbr) {
        printf("storage: Failed to allocate buffer for MBR\n");
        return 0;
    }

    memset(mbr, 0, 512);
    if (storage_read_sectors(device_id, 0, 1, mbr) != 0) {
        printf("storage: Failed to read MBR for partition scan\n");
        kfree(mbr);
        return 0;
    }
    // Partition table starts at offset 446
    for (int i = 0; i < 4; ++i) {
        uint8_t *entry = mbr + 446 + i * 16;
        uint8_t part_type = entry[4];
        uint32_t lba = entry[8] | (entry[9] << 8) | (entry[10] << 16) | (entry[11] << 24);
        printf("storage: Partition %d type=0x%02x lba=%u\n", i, part_type, lba);
        if (part_type == 0x0B || part_type == 0x0C) {
            printf("storage: Found FAT32 partition at LBA %u\n", lba);
            kfree(mbr);
            return lba;
        }
        if (part_type == 0xEE) {
            uint32_t g = storage_gpt_find_fat32(device_id, mbr);
            kfree(mbr);
            return g;
        }
    }
    printf("storage: No FAT32 partition found in MBR\n");
    kfree(mbr);
    return 0;
}

void storage_cache_stats(void) {
    uint64_t total = bcache.hits + bcache.misses;
    uint32_t pct = total ? (uint32_t)(bcache.hits * 100 / total) : 0;
    printf("Block cache: %d/%d entries, hits=%llu misses=%llu (%u%% hit rate)\n",
           bcache.count, BCACHE_CAPACITY,
           (unsigned long long)bcache.hits,
           (unsigned long long)bcache.misses, pct);
}

__attribute__((used, aligned(8), section(".limine_reqs")))
static volatile struct limine_kernel_file_request boot_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0
};

static int boot_dev_cached = -2;

/* The live disk is the one Limine loaded the kernel from, not the largest
 * FAT32 volume. Optical boot (Hyper-V DVD on the second IDE channel) must
 * win over a hard disk that happens to be device 0. */
/* "3" -> 3; "usb0" / "nvme0n1" / "SATA0" -> the device whose name starts
 * with that word (case-insensitive). -1 if none. */
int storage_resolve_name(const char *arg) {
    if (!arg || !arg[0]) return -1;
    bool numeric = true;
    for (const char *c = arg; *c; c++) if (*c < '0' || *c > '9') numeric = false;
    if (numeric) {
        int v = 0;
        for (const char *c = arg; *c; c++) v = v * 10 + (*c - '0');
        return v;
    }
    for (uint8_t i = 0; i < storage_device_count; i++) {
        const char *n = storage_devices[i].name;
        size_t k = 0;
        while (arg[k] && n[k]) {
            char a = arg[k], b = n[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (!arg[k] && (n[k] == ' ' || n[k] == '\0'))
            return i;
    }
    return -1;
}

int storage_find_boot_device(void) {
    const struct limine_file *file = NULL;
    uint8_t i;
    int optical = -1;
    int optical_fat = -1;
    int optical_sig = -1;
    int disk_sig = -1;
    int disk_fat = -1;
    uint32_t want;
    int chosen;
    const char *why;

    if (boot_dev_cached != -2)
        return boot_dev_cached;

    if (boot_file_request.response && boot_file_request.response->kernel_file)
        file = boot_file_request.response->kernel_file;
    want = file ? file->mbr_disk_id : 0;

    for (i = 0; i < storage_device_count; i++) {
        storage_device_t *dev = &storage_devices[i];
        uint8_t sec[512];
        int got;
        uint32_t sig = 0;
        int fat = 0;
        int part;
        if (!dev->is_initialized || dev->pci_dev == NULL)
            continue;
        got = storage_read_sectors(dev->device_id, 0, 1, sec) == 0 &&
              sec[510] == 0x55 && sec[511] == 0xAA;
        if (got) {
            sig = (uint32_t)sec[440] |
                  ((uint32_t)sec[441] << 8) |
                  ((uint32_t)sec[442] << 16) |
                  ((uint32_t)sec[443] << 24);
            for (part = 0; part < 4; part++) {
                uint8_t typ = sec[446 + part * 16 + 4];
                uint32_t lba = (uint32_t)sec[446 + part * 16 + 8] |
                               ((uint32_t)sec[446 + part * 16 + 9] << 8) |
                               ((uint32_t)sec[446 + part * 16 + 10] << 16) |
                               ((uint32_t)sec[446 + part * 16 + 11] << 24);
                if ((typ == 0x0B || typ == 0x0C) && lba)
                    fat = 1;
            }
        }
        if (dev->optical) {
            if (optical < 0)
                optical = dev->device_id;
            if (fat && optical_fat < 0)
                optical_fat = dev->device_id;
            if (got && want && sig == want)
                optical_sig = dev->device_id;
        } else {
            if (got && want && sig == want && disk_sig < 0)
                disk_sig = dev->device_id;
            if (fat && disk_fat < 0)
                disk_fat = dev->device_id;
        }
    }

    chosen = -1;
    why = "none";
    if (file && file->media_type == LIMINE_MEDIA_TYPE_OPTICAL) {
        if (optical_sig >= 0) {
            chosen = optical_sig;
            why = "Limine optical boot, MBR id match";
        } else if (optical_fat >= 0) {
            chosen = optical_fat;
            why = "Limine optical boot, FAT32 DVD";
        } else if (optical >= 0) {
            chosen = optical;
            why = "Limine optical boot, ATAPI device";
        } else {
            why = "Limine optical boot, but no ATAPI DVD was found";
        }
    } else if (want && disk_sig >= 0) {
        chosen = disk_sig;
        why = "Limine MBR disk id";
    } else if (want && optical_sig >= 0) {
        chosen = optical_sig;
        why = "Limine MBR disk id on DVD";
    } else if (!file && optical_fat >= 0) {
        chosen = optical_fat;
        why = "FAT32 DVD (Limine reported no kernel file)";
    } else if (disk_fat >= 0) {
        /* Device order follows PCI order, so a blank or foreign NVMe disk
         * can be device 0. Prefer a disk that actually holds a FAT32 volume. */
        chosen = disk_fat;
        why = "no Limine disk id match; first disk with a FAT32 partition";
    } else if (storage_device_count > 0) {
        chosen = 0;
        why = "no Limine disk id; device 0";
    }

    if (file) {
        printf("storage: Limine boot file media=%u (0=disk 1=optical) mbr_id=%08x\n",
               file->media_type, file->mbr_disk_id);
    } else {
        printf("storage: Limine did not report the kernel file\n");
    }
    if (chosen >= 0) {
        storage_device_t *d = storage_get_device((uint8_t)chosen);
        printf("storage: boot disk is device %d (%s) [%s]\n",
               chosen, d ? d->name : "?", why);
    } else {
        printf("storage: boot disk not found (%s)\n", why);
    }
    boot_dev_cached = chosen;
    return chosen;
}

/* blktest: write a pattern, read it back uncached, compare, restore the old
 * data. Covers single sectors, unaligned starts/lengths (4K-LBA read-modify-
 * write) and transfers larger than one driver command. Destructive only if
 * the restore fails. */
int storage_selftest(uint8_t device_id, uint64_t base) {
    static const uint32_t lens[] = { 1, 7, 8, 9, 64, 300, 513, 1024, 2051 };
    static const uint32_t offs[] = { 0, 3, 8, 1,  5,  16,  0,    7,  1 };
    storage_device_t *dev = storage_get_device(device_id);
    uint32_t maxn = 2051 + 16;
    if (!dev) { printf("blktest: no device %u\n", device_id); return -1; }
    if (base + 4096 + maxn > dev->sector_count) base = 0;
    uint8_t *orig = kalloc(maxn * 512u), *pat = kalloc(maxn * 512u), *chk = kalloc(maxn * 512u);
    int fails = 0, total = 0;
    if (!orig || !pat || !chk) { printf("blktest: out of memory\n"); goto out; }
    printf("blktest: device %u (%s), base LBA %llu\n", device_id, dev->name, (unsigned long long)base);
    for (unsigned t = 0; t < sizeof(lens) / sizeof(lens[0]); t++) {
        uint64_t lba = base + 256u * t + offs[t];
        uint32_t n = lens[t];
        uint64_t t0 = ktime_now_ms();
        total++;
        if (storage_read_sectors(device_id, lba, n, orig) != 0) { printf("blktest: read orig failed lba %llu n %u\n", (unsigned long long)lba, n); fails++; continue; }
        for (uint32_t i = 0; i < n * 512u; i++)
            pat[i] = (uint8_t)((i * 7u) ^ (uint32_t)(lba + i / 512u) ^ (t * 0x35u));
        if (storage_write_sectors(device_id, lba, n, pat) != 0) { printf("blktest: write failed lba %llu n %u\n", (unsigned long long)lba, n); fails++; continue; }
        storage_flush(device_id);
        storage_bcache_invalidate_all();
        memset(chk, 0xA5, n * 512u);
        int ok = storage_read_sectors(device_id, lba, n, chk) == 0 && memcmp(chk, pat, n * 512u) == 0;
        /* neighbours of an unaligned range must be untouched */
        if (storage_write_sectors(device_id, lba, n, orig) != 0) { printf("blktest: RESTORE FAILED lba %llu n %u\n", (unsigned long long)lba, n); ok = 0; }
        storage_bcache_invalidate_all();
        if (ok && (storage_read_sectors(device_id, lba, n, chk) != 0 || memcmp(chk, orig, n * 512u) != 0)) ok = 0;
        printf("blktest: lba %llu n %u: %s (%llu ms)\n", (unsigned long long)lba, n, ok ? "ok" : "MISMATCH",
               (unsigned long long)(ktime_now_ms() - t0));
        if (!ok) fails++;
    }
    storage_flush(device_id);
    printf("BLKTEST %s dev=%u cases=%d fail=%d\n", fails ? "FAIL" : "OK", device_id, total, fails);
out:
    if (orig) kfree(orig);
    if (pat) kfree(pat);
    if (chk) kfree(chk);
    return fails ? -1 : 0;
}
