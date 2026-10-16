/*
 * install.c - In-OS installer for inDevOS
 *
 * Copies the live FAT root onto a second disk and makes that disk boot
 * by itself: MBR + Limine BIOS + FAT32 (default, also carries
 * EFI/BOOT/BOOTX64.EFI), or "gpt": GPT + EFI System Partition for UEFI.
 * The live disk is never formatted.
 *
 * The running system is the hybrid install image mounted as /. That root
 * holds /bin (shell utils and one doom.exe), /doom1.wad, and /install
 * (boot.bin, limine.cfg, limine-bios.sys). Those boot files are also
 * written to /boot and /boot/limine, which is where limine.cfg looks.
 */

#include "install.h"
#include "kprof.h"
#include "ktime.h"
#include "fat32.h"
#include "vfs.h"
#include "vfs_fat32.h"
#include "storage.h"
#include "serial.h"
#include "mem.h"
#include "string.h"
#include "tui.h"
#include "stdio.h"
#include "limine-bios-hdd.h"

#define INSTALL_PART_START       2048u
/* 64 KiB clusters and a 32 MiB in-memory FAT: about 512 GiB. Matches fat32_format. */
#define INSTALL_MAX_PART_SECTORS (128u * ((32u * 1024u * 1024u) / 4u))
#define INSTALL_MIN_PART_SECTORS 131072u  /* 64 MiB: Limine needs >= 65525 clusters */
/* The live root holds the shell tools plus the TCC runtime (about 250
 * headers and libraries), so leave plenty of room. Both limits are hard
 * errors before anything is formatted, never a silent truncation. */
#define INSTALL_MAX_NODES        4096
#define INSTALL_PATH_MAX         192
#define INSTALL_FILE_MAX         (16u * 1024u * 1024u)
#define INSTALL_PROGRESS_EVERY   50
#define INSTALL_DIR_ENTS         VFS_READDIR_MAX

extern void mount_filesystem_device(uint8_t device_id);

typedef struct {
    char path[INSTALL_PATH_MAX];
    uint8_t is_dir;
    void *data;
    uint32_t size;
} install_node_t;

static int path_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int path_ends_ci(const char *path, const char *suf) {
    size_t lp, ls, i;
    if (!path || !suf) return 0;
    lp = strlen(path);
    ls = strlen(suf);
    if (lp < ls) return 0;
    for (i = 0; i < ls; i++) {
        char ca = path[lp - ls + i];
        char cb = suf[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

static int path_has_ci(const char *path, const char *needle) {
    size_t i, nlen, plen;
    if (!path || !needle) return 0;
    nlen = strlen(needle);
    plen = strlen(path);
    if (nlen == 0 || plen < nlen) return 0;
    for (i = 0; i + nlen <= plen; i++) {
        size_t j;
        int ok = 1;
        for (j = 0; j < nlen; j++) {
            char ca = path[i + j];
            char cb = needle[j];
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

extern void unmount_filesystem(void);

static int install_live_id(uint8_t *out) {
    int boot = storage_find_boot_device();
    if (boot < 0)
        return -1;
    *out = (uint8_t)boot;
    /* / must be the disk Limine booted, even if device 0 was mounted or
     * the user unmounted it. The hard disk is the install target. */
    if (!fat32_is_mounted() || !fat32_filesystem.storage_dev ||
        fat32_filesystem.storage_dev->device_id != (uint8_t)boot) {
        if (vfs_has_root_mount() || fat32_is_mounted())
            unmount_filesystem();
        mount_filesystem_device((uint8_t)boot);
    }
    if (!fat32_is_mounted() || !fat32_filesystem.storage_dev ||
        fat32_filesystem.storage_dev->device_id != (uint8_t)boot)
        return -1;
    return 0;
}

static int install_target_ok(storage_device_t *dev, uint8_t live) {
    if (!dev || !dev->is_initialized) return 0;
    if (dev->device_id == live) return 0;
    if (dev->optical) return 0;      /* DVD/ISO is the source, never a target */
    if (dev->pci_dev == NULL) return 0; /* RAM disk */
    if (dev->sector_size != 0 && dev->sector_size != 512) return 0;
    if (dev->sector_count <= INSTALL_PART_START + INSTALL_MIN_PART_SECTORS) return 0;
    return 1;
}

static void install_detach_root(void) {
    vfs_unmount("/");
    if (fat32_is_mounted())
        fat32_unmount();
}

static int install_mount_only(uint8_t id) {
    install_detach_root();
    if (fat32_mount(id) != 0) return -1;
    if (vfs_fat32_register("/") != 0) return -1;
    return 0;
}

static void install_restore_live(uint8_t live) {
    install_detach_root();
    mount_filesystem_device(live);
}

static int install_parse_id(const char *s, int *out) {
    int v = 0;
    if (!s || s[0] < '0' || s[0] > '9') return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 255) return -1;
    }
    *out = v;
    return 0;
}

static int install_join(char *dst, size_t dstsz, const char *dir, const char *name) {
    int n;
    if (!dst || !dir || !name || name[0] == '\0') return -1;
    if (dir[0] == '/' && dir[1] == '\0')
        n = snprintf(dst, dstsz, "/%s", name);
    else
        n = snprintf(dst, dstsz, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= dstsz) return -1;
    return 0;
}

static int install_add(install_node_t *nodes, int *count, const char *path, int is_dir) {
    install_node_t *n;
    size_t len;
    if (*count >= INSTALL_MAX_NODES) {
        printf("install: more than %d files and directories in the live root\n",
               INSTALL_MAX_NODES);
        return -1;
    }
    len = strlen(path);
    if (len == 0 || len >= INSTALL_PATH_MAX) {
        printf("install: path too long: %s\n", path);
        return -1;
    }
    n = &nodes[*count];
    memset(n, 0, sizeof(*n));
    memcpy(n->path, path, len + 1);
    n->is_dir = is_dir ? 1 : 0;
    (*count)++;
    return 0;
}

/* Collect every directory and file under `dir`. The listing buffer is freed
 * before recursing, so deep trees do not hold one big buffer per level.
 * Directories are added before their contents, which is the order the
 * copy loop creates them in. */
static int install_walk(install_node_t *nodes, int *count, const char *dir, int depth) {
    vfs_dirent_t *ents;
    int nent, i, first, last;
    if (depth > 8) {
        printf("install: %s is nested too deeply\n", dir);
        return -1;
    }
    ents = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * INSTALL_DIR_ENTS);
    if (!ents) {
        printf("install: out of memory listing %s\n", dir);
        return -1;
    }
    nent = vfs_readdir(dir, ents, INSTALL_DIR_ENTS);
    if (nent < 0) {
        kfree(ents);
        printf("install: cannot list %s\n", dir);
        return -1;
    }
    if (nent >= INSTALL_DIR_ENTS) {
        kfree(ents);
        printf("install: directory %s has more than %d entries\n", dir, INSTALL_DIR_ENTS - 1);
        return -1;
    }

    first = *count;
    for (i = 0; i < nent; i++) {
        char child[INSTALL_PATH_MAX];
        int is_dir = (ents[i].type == VFS_DIRECTORY);
        if (ents[i].name[0] == '\0') continue;
        if (ents[i].name[0] == '.' &&
            (ents[i].name[1] == '\0' ||
             (ents[i].name[1] == '.' && ents[i].name[2] == '\0')))
            continue;
        if (install_join(child, sizeof(child), dir, ents[i].name) != 0) {
            printf("install: path too long: %s/%s\n", dir, ents[i].name);
            kfree(ents);
            return -1;
        }
        if (!is_dir) {
            /* The install image ships Doom once, at /bin/doom.exe, and the WAD
             * once, at /doom1.wad. Skip any other copy so it is not read. */
            if (path_ends_ci(child, "/doom.exe") && !path_eq_ci(child, "/bin/doom.exe"))
                continue;
            if (path_ends_ci(child, "/doom1.wad") && !path_eq_ci(child, "/doom1.wad"))
                continue;
        }
        if (install_add(nodes, count, child, is_dir) != 0) {
            kfree(ents);
            return -1;
        }
    }
    kfree(ents);

    last = *count;
    for (i = first; i < last; i++) {
        if (!nodes[i].is_dir) continue;
        if (install_walk(nodes, count, nodes[i].path, depth + 1) != 0)
            return -1;
    }
    return 0;
}

static void install_free_nodes(install_node_t *nodes, int count) {
    int i;
    if (!nodes) return;
    for (i = 0; i < count; i++) {
        if (nodes[i].data) {
            kfree(nodes[i].data);
            nodes[i].data = NULL;
        }
    }
    kfree(nodes);
}

static int install_write_partition(uint8_t device_id) {
    storage_device_t *dev = storage_get_device(device_id);
    uint32_t part;
    uint8_t *mbr;
    uint8_t *e;
    int rc;

    if (!dev || dev->sector_count <= INSTALL_PART_START + INSTALL_MIN_PART_SECTORS) {
        printf("install: device %u is too small\n", device_id);
        return -1;
    }
    part = dev->sector_count - INSTALL_PART_START;
    if (part > INSTALL_MAX_PART_SECTORS) {
        part = INSTALL_MAX_PART_SECTORS;
        printf("install: disk is larger than the in-memory FAT can mount.\n");
        printf("install: using the first %u MiB only.\n",
               (unsigned)(((uint64_t)part * 512ull) / (1024ull * 1024ull)));
    }

    mbr = (uint8_t *)kalloc(512);
    if (!mbr) return -1;
    memset(mbr, 0, 512);
    mbr[440] = 0x49;
    mbr[441] = 0x4E;
    mbr[442] = 0x44;
    mbr[443] = 0x56;
    e = mbr + 446;
    e[0] = 0x80;
    e[1] = 0x01;
    e[2] = 0x01;
    e[3] = 0x00;
    e[4] = 0x0C;
    e[5] = 0xFE;
    e[6] = 0xFF;
    e[7] = 0xFF;
    *(uint32_t *)(e + 8) = INSTALL_PART_START;
    *(uint32_t *)(e + 12) = part;
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    rc = storage_write_sectors(device_id, 0, 1, mbr);
    kfree(mbr);
    if (rc != 0) {
        printf("install: failed to write the partition table\n");
        return -1;
    }
    printf("install: partitioned device %u as FAT32 LBA %u, %u sectors\n",
           device_id, INSTALL_PART_START, part);
    return 0;
}

static int write_limine_mbr(uint8_t device_id) {
    uint8_t mbr[512];
    uint8_t orig_pt[70];
    uint8_t sector[512];
    size_t limine_size = sizeof(binary_limine_hdd_bin_data);
    size_t stage2_size = (limine_size > 512) ? (limine_size - 512) : 0;
    size_t stage2_sectors = (stage2_size + 511) / 512;
    size_t i;

    if (1 + stage2_sectors > INSTALL_PART_START) {
        printf("install: Limine stage2 does not fit before the partition\n");
        return -1;
    }
    if (storage_read_sectors(device_id, 0, 1, mbr) != 0)
        return -1;

    /* Same layout as limine bios-install for an MBR disk: stage2 is split
     * into two halves in the gap after the MBR, and the boot sector is told
     * where they are (offsets 0x1a4..0x1b7). The stock blob leaves those
     * fields zero, so without this patch the disk does not boot. */
    size_t size_a_bytes = (stage2_sectors / 2) * 512 + ((stage2_sectors % 2) ? 512 : 0);
    size_t size_b_bytes = (stage2_sectors / 2) * 512;
    uint64_t loc_a = 512;
    uint64_t loc_b = loc_a + (uint64_t)size_a_bytes;
    uint16_t size_a_le, size_b_le;
    if (size_a_bytes > 65535 || size_b_bytes > 65535) {
        printf("install: Limine stage2 is too large for the MBR boot sector\n");
        return -1;
    }
    size_a_le = (uint16_t)size_a_bytes;
    size_b_le = (uint16_t)size_b_bytes;

    memcpy(orig_pt, mbr + 440, 70);
    memcpy(mbr, binary_limine_hdd_bin_data, 512);
    memcpy(mbr + 0x1a4, &size_a_le, sizeof(size_a_le));
    memcpy(mbr + 0x1a6, &size_b_le, sizeof(size_b_le));
    memcpy(mbr + 0x1a8, &loc_a, sizeof(loc_a));
    memcpy(mbr + 0x1b0, &loc_b, sizeof(loc_b));
    memcpy(mbr + 440, orig_pt, 70);

    if (storage_write_sectors(device_id, 0, 1, mbr) != 0)
        return -1;

    for (i = 0; i < stage2_sectors; i++) {
        size_t off = 512 + i * 512;
        size_t chunk = 512;
        if (off >= limine_size) break;
        if (off + chunk > limine_size) chunk = limine_size - off;
        memset(sector, 0, sizeof(sector));
        memcpy(sector, binary_limine_hdd_bin_data + off, chunk);
        if (storage_write_sectors(device_id, 1 + (uint32_t)i, 1, sector) != 0)
            return -1;
    }
    return 0;
}

/* ---- GPT (UEFI) layout -------------------------------------------------
 * LBA 0 protective MBR, LBA 1 header, LBA 2..33 128 entries, partition 1
 * (EFI System, FAT32) from INSTALL_PART_START, backup array and header at
 * the end of the disk. The ESP is also the root filesystem. */
#define GPT_ENTRIES      128u
#define GPT_ENTRY_SIZE   128u
#define GPT_ARRAY_SECT   ((GPT_ENTRIES * GPT_ENTRY_SIZE) / 512u)   /* 32 */

static const uint8_t install_gpt_esp_type[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };

static uint32_t install_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void install_guid(uint8_t g[16], uint64_t seed) {
    int i;
    for (i = 0; i < 16; i++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        g[i] = (uint8_t)(seed >> 24);
    }
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);   /* RFC 4122 variant */
}

static void install_gpt_header(uint8_t *h, uint64_t my_lba, uint64_t alt_lba,
                               uint64_t first, uint64_t last, uint64_t array_lba,
                               const uint8_t disk_guid[16], uint32_t array_crc) {
    memset(h, 0, 512);
    memcpy(h, "EFI PART", 8);
    *(uint32_t *)(h + 8)  = 0x00010000u;
    *(uint32_t *)(h + 12) = 92;
    *(uint64_t *)(h + 24) = my_lba;
    *(uint64_t *)(h + 32) = alt_lba;
    *(uint64_t *)(h + 40) = first;
    *(uint64_t *)(h + 48) = last;
    memcpy(h + 56, disk_guid, 16);
    *(uint64_t *)(h + 72) = array_lba;
    *(uint32_t *)(h + 80) = GPT_ENTRIES;
    *(uint32_t *)(h + 84) = GPT_ENTRY_SIZE;
    *(uint32_t *)(h + 88) = array_crc;
    *(uint32_t *)(h + 16) = install_crc32(h, 92);
}

static int install_write_gpt(uint8_t device_id, uint32_t *out_start, uint32_t *out_size) {
    storage_device_t *dev = storage_get_device(device_id);
    uint64_t total, last_usable, part_end;
    uint32_t part;
    uint8_t *buf, *arr, *e;
    uint8_t disk_guid[16], part_guid[16];
    uint32_t arr_crc;
    uint64_t seed;
    int rc = -1;
    static const char name[] = "inDevOS";
    size_t i;

    if (!dev) return -1;
    total = dev->sector_count;
    if (total <= INSTALL_PART_START + INSTALL_MIN_PART_SECTORS + 1 + GPT_ARRAY_SECT) {
        printf("install: device %u is too small\n", device_id);
        return -1;
    }
    last_usable = total - 2 - GPT_ARRAY_SECT;
    part_end = last_usable;
    if (part_end - INSTALL_PART_START + 1 > INSTALL_MAX_PART_SECTORS) {
        part_end = INSTALL_PART_START + INSTALL_MAX_PART_SECTORS - 1;
        printf("install: disk is larger than the in-memory FAT can mount; using the first %u MiB.\n",
               (unsigned)(((uint64_t)INSTALL_MAX_PART_SECTORS * 512ull) >> 20));
    }
    part = (uint32_t)(part_end - INSTALL_PART_START + 1);

    buf = (uint8_t *)kalloc(512);
    arr = (uint8_t *)kalloc(GPT_ARRAY_SECT * 512u);
    if (!buf || !arr) goto out;

    seed = ktime_now_us() ^ (total * 0x9E3779B97F4A7C15ull) ^ 0xD1B54A32D192ED03ull;
    install_guid(disk_guid, seed);
    install_guid(part_guid, seed * 31u + 7u);

    memset(arr, 0, GPT_ARRAY_SECT * 512u);
    e = arr;
    memcpy(e, install_gpt_esp_type, 16);
    memcpy(e + 16, part_guid, 16);
    *(uint64_t *)(e + 32) = INSTALL_PART_START;
    *(uint64_t *)(e + 40) = part_end;
    *(uint64_t *)(e + 48) = 0;
    for (i = 0; name[i]; i++)
        *(uint16_t *)(e + 56 + i * 2) = (uint16_t)name[i];
    arr_crc = install_crc32(arr, GPT_ARRAY_SECT * 512u);

    /* Protective MBR */
    memset(buf, 0, 512);
    buf[446 + 1] = 0x00; buf[446 + 2] = 0x02; buf[446 + 3] = 0x00;
    buf[446 + 4] = 0xEE;
    buf[446 + 5] = 0xFF; buf[446 + 6] = 0xFF; buf[446 + 7] = 0xFF;
    *(uint32_t *)(buf + 446 + 8) = 1;
    *(uint32_t *)(buf + 446 + 12) = (total - 1 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)(total - 1);
    buf[510] = 0x55; buf[511] = 0xAA;
    if (storage_write_sectors(device_id, 0, 1, buf) != 0) goto out;

    /* Primary array + header */
    if (storage_write_sectors(device_id, 2, GPT_ARRAY_SECT, arr) != 0) goto out;
    install_gpt_header(buf, 1, total - 1, 34, last_usable, 2, disk_guid, arr_crc);
    if (storage_write_sectors(device_id, 1, 1, buf) != 0) goto out;

    /* Backup array + header */
    if (storage_write_sectors(device_id, (uint32_t)(total - 1 - GPT_ARRAY_SECT),
                              GPT_ARRAY_SECT, arr) != 0) goto out;
    install_gpt_header(buf, total - 1, 1, 34, last_usable, total - 1 - GPT_ARRAY_SECT,
                       disk_guid, arr_crc);
    if (storage_write_sectors(device_id, (uint32_t)(total - 1), 1, buf) != 0) goto out;

    /* Clear the old FAT boot sector so a stale volume is not picked up. */
    memset(buf, 0, 512);
    storage_write_sectors(device_id, INSTALL_PART_START, 1, buf);

    *out_start = INSTALL_PART_START;
    *out_size = part;
    printf("install: GPT on device %u: EFI System Partition LBA %u, %u sectors\n",
           device_id, INSTALL_PART_START, part);
    rc = 0;
out:
    if (rc != 0) printf("install: failed to write the GPT\n");
    if (buf) kfree(buf);
    if (arr) kfree(arr);
    return rc;
}

static void install_usage(void) {
    printf("Usage: install [device_id] [yes] [mbr|gpt]\n");
    printf("  mbr (default): MBR + Limine BIOS, plus EFI/BOOT/BOOTX64.EFI so\n");
    printf("                 UEFI firmware can boot the same disk.\n");
    printf("  gpt:           GPT + EFI System Partition, UEFI boot only.\n");
    printf("  Copies this live root onto another disk: /bin, Doom, the kernel,\n");
    printf("  and Limine. Rewrites that disk as one bootable FAT32 partition.\n");
    printf("  Asks before wiping unless 'yes' is given. Never formats the live disk.\n");
}

void cmd_install(int argc, char **argv) {
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    uint8_t live = 0;
    uint8_t target = 0xFF;
    int got_target = 0;
    int assume_yes = 0;
    int use_gpt = 0;
    uint32_t gpt_start = 0, gpt_size = 0;
    void *boot_efi = NULL;
    uint32_t boot_efi_sz = 0;
    int i;
    install_node_t *nodes = NULL;
    int count = 0;
    int saw_util = 0, saw_doom = 0, saw_wad = 0;
    void *boot_bin = NULL, *boot_cfg = NULL, *boot_sys = NULL;
    uint32_t boot_bin_sz = 0, boot_cfg_sz = 0, boot_sys_sz = 0;
    uint32_t total_bytes = 0;
    int use_tui;
    int failed = 0;

    if (install_live_id(&live) != 0) {
        printf("No filesystem mounted. Boot the install image as a disk first.\n");
        return;
    }

    for (i = 1; i < argc; i++) {
        if (kstrcmp(argv[i], "yes") == 0 || kstrcmp(argv[i], "-y") == 0 ||
            kstrcmp(argv[i], "--yes") == 0) {
            assume_yes = 1;
        } else if (kstrcmp(argv[i], "gpt") == 0 || kstrcmp(argv[i], "uefi") == 0) {
            use_gpt = 1;
        } else if (kstrcmp(argv[i], "mbr") == 0 || kstrcmp(argv[i], "bios") == 0) {
            use_gpt = 0;
        } else if (kstrcmp(argv[i], "help") == 0 || kstrcmp(argv[i], "-h") == 0) {
            install_usage();
            return;
        } else if (!got_target) {
            int dev = 0;
            if (install_parse_id(argv[i], &dev) != 0 ||
                dev >= (int)storage_device_count) {
                printf("Invalid device ID: %s\n", argv[i]);
                install_usage();
                return;
            }
            target = (uint8_t)dev;
            got_target = 1;
        } else {
            printf("Unexpected argument: %s\n", argv[i]);
            install_usage();
            return;
        }
    }

    printf("install: live disk is device %u (%s). It will not be formatted.\n",
           live, storage_get_device(live) ? storage_get_device(live)->name : "?");
    for (i = 0; i < (int)storage_device_count; i++) {
        storage_device_t *dev = &storage_devices[i];
        uint64_t size_mb;
        if (!dev->is_initialized) continue;
        size_mb = ((uint64_t)dev->sector_count * (dev->sector_size ? dev->sector_size : 512))
                  / (1024ull * 1024ull);
        printf("install: device %u: %s (%llu MB)%s\n",
               dev->device_id, dev->name, (unsigned long long)size_mb,
               install_target_ok(dev, live) ? "" : " [not a target]");
    }

    if (!got_target) {
        tui_menu_t *menu = tui_menu_create("inDevOS Installer",
            "Select a disk. The live disk is not listed.");
        if (!menu) {
            printf("Out of memory.\n");
            return;
        }
        tui_menu_set_theme(menu, TUI_THEME_CYAN);
        tui_menu_add_button(menu, "Select", TUI_KEY_ENTER);
        tui_menu_add_button(menu, "Exit", TUI_KEY_ESCAPE);
        for (i = 0; i < (int)storage_device_count; i++) {
            storage_device_t *dev = &storage_devices[i];
            uint64_t size_mb;
            char label[80];
            if (!install_target_ok(dev, live)) continue;
            size_mb = ((uint64_t)dev->sector_count * (dev->sector_size ? dev->sector_size : 512))
                      / (1024ull * 1024ull);
            snprintf(label, sizeof(label), "Device %u: %s (%llu MB)",
                     dev->device_id, dev->name, (unsigned long long)size_mb);
            tui_menu_add(menu, label, TUI_ITEM_TEXT, dev->device_id);
        }
        if (menu->item_count == 0) {
            tui_menu_destroy(menu);
            printf("install: no target disk. Attach a second drive and run install again.\n");
            return;
        }
        {
            int result = tui_menu_run(menu);
            target = (uint8_t)tui_menu_get_selected_tag(menu);
            tui_menu_destroy(menu);
            if (result != TUI_RESULT_SELECT) {
                printf("Installation cancelled.\n");
                return;
            }
        }
    }

    if (target == live || !install_target_ok(storage_get_device(target), live)) {
        printf("install: refusing device %u. Pick a real disk that is not the live one.\n",
               target);
        return;
    }

    if (!assume_yes) {
        char confirm_msg[160];
        snprintf(confirm_msg, sizeof(confirm_msg),
                 "ERASE device %u and install inDevOS? Live device %u is not touched.",
                 target, live);
        if (!tui_yes_no("Confirm Installation", confirm_msg)) {
            printf("Installation cancelled.\n");
            return;
        }
    } else {
        printf("install: confirmation given on the command line (yes).\n");
    }

    use_tui = !assume_yes;
    if (use_tui) tui_progress_begin("inDevOS Installer");

    if (use_tui) tui_progress_step(1, 5, "Reading the live root...");
    printf("install: reading the live root.\n");
    kprof_reset();
    uint64_t t_install0 = ktime_now_us();
    uint64_t kp_t;

    nodes = (install_node_t *)kalloc(sizeof(install_node_t) * INSTALL_MAX_NODES);
    if (!nodes) {
        if (use_tui) tui_progress_finish("Out of memory.");
        else printf("install: out of memory.\n");
        return;
    }
    memset(nodes, 0, sizeof(install_node_t) * INSTALL_MAX_NODES);

    kp_t = kprof_begin();
    int walk_rc = install_walk(nodes, &count, "/", 0);
    kprof_end(KP_INST_WALK, kp_t, 0);
    if (walk_rc != 0) {
        install_free_nodes(nodes, count);
        if (use_tui) tui_progress_finish("Could not read the live directory tree.");
        else printf("install: could not read the live directory tree.\n");
        return;
    }
    /* Per-file lines go to the serial log only; the console gets a progress
     * line every INSTALL_PROGRESS_EVERY files. Each console line scrolls the
     * framebuffer, which under QEMU cost ~0.4 s, more than the file I/O. */
    int n_files = 0, n_done = 0;
    for (i = 0; i < count; i++)
        if (!nodes[i].is_dir) n_files++;
    printf("install: %d directories and %d files to copy.\n", count - n_files, n_files);

    for (i = 0; i < count; i++) {
        int n;
        if (nodes[i].is_dir) continue;
        kp_t = kprof_begin();
        n = vfs_read_file_alloc(nodes[i].path, &nodes[i].data, INSTALL_FILE_MAX);
        kprof_end(KP_INST_READ, kp_t, n > 0 ? ((uint32_t)n + 511u) / 512u : 0);
        if (n < 0 || !nodes[i].data) {
            printf("install: failed to read %s\n", nodes[i].path);
            install_free_nodes(nodes, count);
            if (use_tui) tui_progress_finish("Failed while reading the live root.");
            else printf("install: aborted before formatting.\n");
            return;
        }
        nodes[i].size = (uint32_t)n;
        total_bytes += nodes[i].size;
        kp_t = kprof_begin();
        serial_printf("install: read %s (%u bytes)\n", nodes[i].path, nodes[i].size);
        n_done++;
        if (n_done % INSTALL_PROGRESS_EVERY == 0 || n_done == n_files)
            printf("install: read %d/%d files (%u KB)\n", n_done, n_files, total_bytes / 1024u);
        kprof_end(KP_INST_LOG, kp_t, 0);

        if (path_has_ci(nodes[i].path, "/bin/") && path_ends_ci(nodes[i].path, ".exe") &&
            !path_ends_ci(nodes[i].path, "/doom.exe"))
            saw_util = 1;
        if (path_ends_ci(nodes[i].path, "/doom.exe"))
            saw_doom = 1;
        if (path_ends_ci(nodes[i].path, "/doom1.wad"))
            saw_wad = 1;
    }

    for (i = 0; i < count; i++) {
        if (nodes[i].is_dir) continue;
        if (path_eq_ci(nodes[i].path, "/install/boot.bin")) {
            boot_bin = nodes[i].data;
            boot_bin_sz = nodes[i].size;
        } else if (path_eq_ci(nodes[i].path, "/boot/boot.bin") && !boot_bin) {
            boot_bin = nodes[i].data;
            boot_bin_sz = nodes[i].size;
        }
        if (path_eq_ci(nodes[i].path, "/install/limine.cfg")) {
            boot_cfg = nodes[i].data;
            boot_cfg_sz = nodes[i].size;
        } else if (path_eq_ci(nodes[i].path, "/boot/limine/limine.cfg") && !boot_cfg) {
            boot_cfg = nodes[i].data;
            boot_cfg_sz = nodes[i].size;
        }
        if (path_eq_ci(nodes[i].path, "/install/BOOTX64.EFI")) {
            boot_efi = nodes[i].data;
            boot_efi_sz = nodes[i].size;
        } else if (path_eq_ci(nodes[i].path, "/EFI/BOOT/BOOTX64.EFI") && !boot_efi) {
            boot_efi = nodes[i].data;
            boot_efi_sz = nodes[i].size;
        }
        if (path_eq_ci(nodes[i].path, "/install/limine-bios.sys")) {
            boot_sys = nodes[i].data;
            boot_sys_sz = nodes[i].size;
        } else if (path_eq_ci(nodes[i].path, "/boot/limine/limine-bios.sys") && !boot_sys) {
            boot_sys = nodes[i].data;
            boot_sys_sz = nodes[i].size;
        }
    }

    if (!boot_bin || !boot_cfg || !boot_sys || !saw_util || !saw_doom || !saw_wad) {
        printf("install: live root is incomplete (kernel=%d cfg=%d limine=%d utils=%d doom=%d wad=%d).\n",
               boot_bin ? 1 : 0, boot_cfg ? 1 : 0, boot_sys ? 1 : 0,
               saw_util, saw_doom, saw_wad);
        printf("install: boot the hybrid install image. Nothing was erased.\n");
        install_free_nodes(nodes, count);
        if (use_tui) tui_progress_finish("Live root is missing files. Nothing was erased.");
        return;
    }
    if (use_gpt && !boot_efi) {
        printf("install: gpt needs /install/BOOTX64.EFI in the live root. Nothing was erased.\n");
        install_free_nodes(nodes, count);
        if (use_tui) tui_progress_finish("No UEFI loader in the live root. Nothing was erased.");
        return;
    }
    if (!boot_efi)
        printf("install: no BOOTX64.EFI in the live root; the disk will boot on BIOS only.\n");
    printf("install: %s layout.\n", use_gpt ? "GPT/UEFI" : "MBR/BIOS (+UEFI)");
    printf("install: %d entries, %u bytes of files. Target device %u.\n",
           count, total_bytes, target);

    if (target == live) {
        install_free_nodes(nodes, count);
        if (use_tui) tui_progress_finish("Refusing to format the live disk.");
        else printf("install: refusing to format the live disk.\n");
        return;
    }

    if (use_tui) tui_progress_step(2, 5, "Partitioning the target...");
    install_detach_root();
    if ((use_gpt ? install_write_gpt(target, &gpt_start, &gpt_size)
                 : install_write_partition(target)) != 0) {
        install_free_nodes(nodes, count);
        install_restore_live(live);
        if (use_tui) tui_progress_finish("Partitioning failed. Live disk restored.");
        else printf("install: partitioning failed. Live disk remounted.\n");
        return;
    }

    if (use_tui) tui_progress_step(3, 5, "Formatting FAT32...");
    if ((use_gpt ? fat32_format_range(target, "INDEVOS", gpt_start, gpt_size)
                 : fat32_format(target, "INDEVOS")) != 0) {
        install_free_nodes(nodes, count);
        install_restore_live(live);
        if (use_tui) tui_progress_finish("Format failed. Live disk restored.");
        else printf("install: format failed. Live disk remounted.\n");
        return;
    }

    if (install_mount_only(target) != 0) {
        install_free_nodes(nodes, count);
        install_restore_live(live);
        if (use_tui) tui_progress_finish("Could not mount the new volume.");
        else printf("install: could not mount the new volume.\n");
        return;
    }
    /* Batch FAT write-back for the whole copy; synced before detaching. */
    fat32_set_deferred_flush(true);

    if (use_tui) tui_progress_step(4, 5, "Copying utils, Doom, kernel...");
    for (i = 0; i < count; i++) {
        int rc;
        if (!nodes[i].is_dir) continue;
        kp_t = kprof_begin();
        serial_printf("install: mkdir %s\n", nodes[i].path);
        kprof_end(KP_INST_LOG, kp_t, 0);
        kp_t = kprof_begin();
        rc = vfs_mkdir(nodes[i].path);
        kprof_end(KP_INST_MKDIR, kp_t, 0);
        if (rc != 0 && !vfs_exists(nodes[i].path)) {
            printf("install: mkdir failed for %s (%d)\n", nodes[i].path, rc);
            failed = 1;
            break;
        }
    }
    if (!failed) {
        uint32_t bytes_done = 0;
        n_done = 0;
        for (i = 0; i < count; i++) {
            int rc;
            if (nodes[i].is_dir) continue;
            kp_t = kprof_begin();
            serial_printf("install: write %s (%u bytes)\n", nodes[i].path, nodes[i].size);
            n_done++;
            bytes_done += nodes[i].size;
            if (n_done % INSTALL_PROGRESS_EVERY == 0 || n_done == n_files)
                printf("install: copied %d/%d files (%u KB)\n", n_done, n_files,
                       bytes_done / 1024u);
            kprof_end(KP_INST_LOG, kp_t, 0);
            kp_t = kprof_begin();
            rc = vfs_write_file(nodes[i].path, nodes[i].data, nodes[i].size);
            kprof_end(KP_INST_WRITE, kp_t, (nodes[i].size + 511u) / 512u);
            if (rc != 0) {
                printf("install: write failed for %s (%d)\n", nodes[i].path, rc);
                failed = 1;
                break;
            }
        }
    }
    if (!failed) {
        printf("install: placing Limine boot files.\n");
        if (vfs_write_file("/boot/boot.bin", boot_bin, boot_bin_sz) != 0 ||
            vfs_write_file("/boot/limine/limine.cfg", boot_cfg, boot_cfg_sz) != 0 ||
            vfs_write_file("/boot/limine/limine-bios.sys", boot_sys, boot_sys_sz) != 0) {
            printf("install: failed to write /boot for Limine.\n");
            failed = 1;
        }
    }
    if (!failed && boot_efi) {
        if ((vfs_mkdir("/EFI") != 0 && !vfs_exists("/EFI")) ||
            (vfs_mkdir("/EFI/BOOT") != 0 && !vfs_exists("/EFI/BOOT")) ||
            vfs_write_file("/EFI/BOOT/BOOTX64.EFI", boot_efi, boot_efi_sz) != 0) {
            printf("install: failed to write /EFI/BOOT/BOOTX64.EFI.\n");
            failed = 1;
        } else {
            printf("install: placed UEFI loader /EFI/BOOT/BOOTX64.EFI.\n");
        }
    }

    install_free_nodes(nodes, count);
    nodes = NULL;
    kp_t = kprof_begin();
    /* Write back everything the copy left in the FAT cache (all FAT copies,
     * FSInfo) before the volume is detached; unmount syncs again. */
    fat32_set_deferred_flush(false);
    if (fat32_sync() != 0) {
        printf("install: writing the FAT to device %u failed.\n", target);
        failed = 1;
    }
    install_detach_root();
    kprof_end(KP_INST_FINAL, kp_t, 0);

    if (failed) {
        install_restore_live(live);
        if (use_tui) tui_progress_finish("Copy failed. The target may be incomplete.");
        else printf("install: copy failed. The target may be incomplete. Live disk remounted.\n");
        return;
    }

    if (use_tui) tui_progress_step(5, 5, use_gpt ? "Finishing the UEFI install..."
                                                 : "Installing Limine into the MBR...");
    if (!use_gpt && write_limine_mbr(target) != 0) {
        install_restore_live(live);
        if (use_tui) tui_progress_finish("Bootloader install failed.");
        else printf("install: Limine install failed. Live disk remounted.\n");
        return;
    }
    kp_t = kprof_begin();
    storage_flush(target);
    kprof_end(KP_INST_FINAL, kp_t, 0);
    install_restore_live(live);
    {
        uint64_t total_ms = (ktime_now_us() - t_install0) / 1000u;
        kprof_slot_t *r = &kprof_slots[KP_INST_READ], *w = &kprof_slots[KP_INST_WRITE];
        printf("install: timing: total %llu ms; read %llu files in %llu ms, wrote %llu files in %llu ms\n",
               (unsigned long long)total_ms,
               (unsigned long long)r->count, (unsigned long long)(r->us / 1000u),
               (unsigned long long)w->count, (unsigned long long)(w->us / 1000u));
        kprof_dump("install");
    }

    printf("install: complete. Device %u is a bootable FAT32 disk with /bin, Doom, and Limine.\n",
           target);
    printf("install: reboot and select that disk. The live disk was not formatted.\n");
    if (use_tui) {
        tui_progress_finish("Installation complete!");
        tui_message_box("Install Complete",
            "inDevOS is on the target drive with utils and Doom.\n\n"
            "Reboot and select that drive. The live disk was not touched.");
    }
}