/*
 * cmd.c - Terminal multiplexer and shared shell utilities for inDevOS
 *
 * This module serves two purposes:
 *
 *  1. TERMINAL MULTIPLEXER
 *     cmd() is the single entry point from the keyboard/input layer.
 *     It dispatches raw command lines to the active terminal backend.
 *     By default, all input is forwarded to Zeus (the primary shell).
 *     Additional backends can be registered with cmd_register_terminal()
 *     and activated with cmd_set_active_terminal().
 *
 *  2. SHARED UTILITY FUNCTIONS
 *     Helper implementations that are called by Zeus and other modules
 *     (filesystem ops, network status, USB status, image display, etc.)
 *     These are legacy helpers; new commands should live in zeus.c.
 *
 * NOTE: All interactive command dispatch has been moved to zeus.c.
 */

#include "cmd.h"
#include "zeus.h"
#include "stdio.h"
#include "utils.h"
#include "mem.h"
#include "string.h"
#include "hpet.h"
#include "storage.h"
#include "fat32.h"
#include "ext4.h"
#include "vfs.h"
#include "vfs_fat32.h"
#include "vfs_ext4.h"
#include "serial.h"
#include "net_device.h"
#include "net_arp.h"
#include "usb.h"
#include "png.h"
#include "simple_img.h"
#include "keyboard.h"
#include "mouse.h"
#include "cpu.h"
#include "amd_gpu.h"
#include "string.h"

/* ================================================================
 * Terminal multiplexer
 * ================================================================ */

#define MAX_TERMINALS 8

typedef struct {
    char            name[32];
    cmd_terminal_fn handler;
    bool            registered;
} terminal_entry_t;

static terminal_entry_t terminal_registry[MAX_TERMINALS];
static int terminal_count  = 0;
static int active_terminal = -1;   /* -1 = use default (zeus) */

int cmd_register_terminal(const char *name, cmd_terminal_fn handler) {
    if (!name || !handler) return -1;

    /* Update existing */
    for (int i = 0; i < terminal_count; i++) {
        if (kstrcmp(terminal_registry[i].name, name) == 0) {
            terminal_registry[i].handler = handler;
            serial_printf("[cmd] terminal '%s' handler updated\n", name);
            return i;
        }
    }

    if (terminal_count >= MAX_TERMINALS) {
        printf("cmd: terminal registry full (max %d)\n", MAX_TERMINALS);
        return -1;
    }

    kstrncpy(terminal_registry[terminal_count].name, name, 31);
    terminal_registry[terminal_count].name[31]    = '\0';
    terminal_registry[terminal_count].handler     = handler;
    terminal_registry[terminal_count].registered  = true;
    serial_printf("[cmd] terminal '%s' registered at index %d\n", name, terminal_count);
    return terminal_count++;
}

int cmd_set_active_terminal(const char *name) {
    if (!name) {
        active_terminal = -1;
        printf("Switched to default terminal (zeus)\n");
        return 0;
    }
    for (int i = 0; i < terminal_count; i++) {
        if (terminal_registry[i].registered &&
            kstrcmp(terminal_registry[i].name, name) == 0) {
            active_terminal = i;
            printf("Switched to terminal: %s\n", name);
            return 0;
        }
    }
    printf("cmd: unknown terminal '%s'\n", name);
    return -1;
}

const char *cmd_get_active_terminal(void) {
    if (active_terminal >= 0 && active_terminal < terminal_count &&
        terminal_registry[active_terminal].registered)
        return terminal_registry[active_terminal].name;
    return "zeus";
}

int cmd_list_terminals(void) {
    const char *active_name = cmd_get_active_terminal();
    printf("Terminal backends:\n");
    printf("  %s zeus (built-in default)\n",
           (active_terminal < 0) ? ">" : " ");
    for (int i = 0; i < terminal_count; i++) {
        if (terminal_registry[i].registered) {
            printf("  %s %s\n",
                   (active_terminal == i) ? ">" : " ",
                   terminal_registry[i].name);
        }
    }
    printf("Active: %s\n", active_name);
    return terminal_count;
}

/* ================================================================
 * Main dispatch entry point
 * ================================================================ */

void cmd(char *input, uint64_t len) {
    DEBUG_SERIAL("cmd() dispatch entered\n");

    if (input == NULL) {
        DEBUG_SERIAL("cmd(): NULL input\n");
        return;
    }
    if (((uintptr_t)input) < 0x1000) {
        printf("cmd(): invalid pointer (%p)\n", input);
        return;
    }

    serial_printf("[cmd] -> '%s' (terminal: %s)\n",
                  input, cmd_get_active_terminal());

    /* Route to the registered active terminal */
    if (active_terminal >= 0 && active_terminal < terminal_count &&
        terminal_registry[active_terminal].registered) {
        terminal_registry[active_terminal].handler(input, (size_t)len);
        __asm__ volatile ("sti");
        return;
    }

    /* Default: route to Zeus shell */
    zeus_process_command_service(input);
    __asm__ volatile ("sti");
}

/* ================================================================
 * Utility functions (shared helpers used by zeus.c and kernel.c)
 * ================================================================ */

void clear(void) {
    clear_screen();
}

void timer_test(void) {
    printf("Starting timer test - waiting 5 seconds...\n");
    uint64_t start = hpet_get_ticks();

    for (int i = 0; i < 5; i++) {
        hpet_sleep_counter(1000);
        printf("Second %d elapsed, total ticks: %llu\n",
               i + 1, (unsigned long long)hpet_get_ticks());
    }

    uint64_t end = hpet_get_ticks();
    printf("Timer test completed. Elapsed ticks: %llu\n",
           (unsigned long long)(end - start));
}

void timer_accuracy_test(void) {
    printf("=== HPET Timer Accuracy Test ===\n");

    extern volatile uint64_t *hpet_main_cnt;
    if (!hpet_main_cnt || !hpet_get_frequency()) {
        printf("No HPET counter; fallback clock is in use.\n");
        uint64_t start = hpet_get_ticks();
        hpet_sleep_counter(1000);
        uint64_t end = hpet_get_ticks();
        printf("1s sleep advanced %llu ms (freq %llu Hz)\n",
               (unsigned long long)(end - start),
               (unsigned long long)hpet_get_frequency());
        printf("=== Timer Accuracy Test Complete ===\n");
        return;
    }

    printf("\nMethod 1: HPET Sleep Counter\n");
    uint64_t hpet_freq = hpet_get_frequency();

    uint64_t t0 = *hpet_main_cnt;
    hpet_sleep_counter(1000);
    uint64_t t1 = *hpet_main_cnt;
    uint64_t elapsed = t1 - t0;
    printf("Expected: %llu ticks  Actual: %llu ticks  Accuracy: %llu%%\n",
           (unsigned long long)hpet_freq,
           (unsigned long long)elapsed,
           (unsigned long long)((elapsed * 100) / hpet_freq));

    printf("\nMethod 2: 10 x 100ms intervals\n");
    uint64_t total = 0;
    for (int i = 0; i < 10; i++) {
        uint64_t a = *hpet_main_cnt;
        hpet_sleep_counter(100);
        uint64_t b = *hpet_main_cnt;
        total += (b - a);
        printf("  Interval %2d: %llu ticks\n", i + 1, (unsigned long long)(b - a));
    }
    printf("Total: %llu  Expected: %llu  Accuracy: %llu%%\n",
           (unsigned long long)total,
           (unsigned long long)hpet_freq,
           (unsigned long long)((total * 100) / hpet_freq));

    printf("\nMethod 3: CPU cycle comparison\n");
    uint64_t cs, ce;
    __asm__ volatile ("rdtsc" : "=A" (cs));
    hpet_sleep_counter(1000);
    __asm__ volatile ("rdtsc" : "=A" (ce));
    printf("CPU cycles/second (approx): %llu  (~%llu MHz)\n",
           (unsigned long long)(ce - cs),
           (unsigned long long)((ce - cs) / 1000000));

    printf("\nMethod 4: Variable intervals\n");
    uint64_t intervals[] = {10, 50, 100, 500, 1000};
    for (int i = 0; i < 5; i++) {
        uint64_t a = *hpet_main_cnt;
        hpet_sleep_counter(intervals[i]);
        uint64_t b = *hpet_main_cnt;
        uint64_t actual   = b - a;
        uint64_t expected = (hpet_freq * intervals[i]) / 1000;
        printf("  %4llums: expected %llu  got %llu  (%llu%%)\n",
               (unsigned long long)intervals[i],
               (unsigned long long)expected,
               (unsigned long long)actual,
               (unsigned long long)((actual * 100) / expected));
    }

    printf("\n=== Timer Accuracy Test Complete ===\n");
}

void stopwatch_test(void) {
    printf("=== Stopwatch Test (10 seconds) ===\n");
    for (int i = 1; i <= 10; i++) {
        hpet_sleep_counter(1000);
        printf("%d... ", i);
        if (i % 5 == 0) printf("\n");
    }
    printf("\nSub-second test (500ms intervals):\n");
    for (int i = 1; i <= 10; i++) {
        hpet_sleep_counter(500);
        printf("Tick %d\n", i);
    }
    printf("Stopwatch test complete!\n");
}

void gpu_test(void) {
    printf("GPU test running...\n");
}

void unmount_filesystem_path(const char *path);

void mount_filesystem(void) {
    int id = storage_find_boot_device();
    if (id < 0) {
        printf("No boot disk found. Boot the install image as a disk first.\n");
        return;
    }
    mount_filesystem_device((uint8_t)id);
}

void mount_filesystem_device(uint8_t device_id) {
    /* Remounting / is common after booting the install DVD and switching to
     * a writable HDD. Drop the existing VFS root first so register succeeds. */
    if (vfs_find_mount("/", NULL)) {
        printf("Unmounting current / before remount...\n");
        unmount_filesystem_path("/");
    }
    printf("Mounting FAT32 filesystem from device %d...\n", device_id);
    if (fat32_mount(device_id) == 0) {
        printf("Filesystem mounted successfully!\n");
        if (vfs_fat32_register("/") == 0) {
            printf("VFS: FAT32 registered at /\n");
            vfs_seed_standard_layout();
            zeus_use_home_if_present();
        }
    } else {
        printf("Failed to mount filesystem from device %d\n", device_id);
    }
}

void mount_filesystem_ext(uint8_t device_id, uint32_t partition_lba,
                          const char *fstype, const char *mount_path) {
    if (!fstype) fstype = "fat32";
    if (!mount_path) mount_path = "/";

    if (kstrcmp(fstype, "ext4") == 0) {
        printf("Mounting ext4 from device %d partition LBA %u at %s...\n",
               device_id, partition_lba, mount_path);
        if (ext4_mount(device_id, partition_lba) == 0) {
            printf("ext4 mounted successfully!\n");
            if (vfs_ext4_register(mount_path) == 0) {
                printf("VFS: ext4 registered at %s\n", mount_path);
                if (mount_path[0] == '/' && mount_path[1] == '\0') {
                    vfs_seed_standard_layout();
                    zeus_use_home_if_present();
                }
            }
        } else {
            printf("Failed to mount ext4 from device %d\n", device_id);
        }
    } else {
        printf("Mounting FAT32 from device %d at %s...\n", device_id, mount_path);
        if (fat32_mount(device_id) == 0) {
            printf("FAT32 mounted successfully!\n");
            if (vfs_fat32_register(mount_path) == 0) {
                printf("VFS: FAT32 registered at %s\n", mount_path);
                if (mount_path[0] == '/' && mount_path[1] == '\0') {
                    vfs_seed_standard_layout();
                    zeus_use_home_if_present();
                }
            }
        } else {
            printf("Failed to mount FAT32 from device %d\n", device_id);
        }
    }
}

void unmount_filesystem_path(const char *path) {
    if (!path) path = "/";

    const char *rel = NULL;
    vfs_filesystem_t *fs = vfs_find_mount(path, &rel);
    if (!fs) {
        printf("No filesystem mounted at '%s'\n", path);
        return;
    }

    vfs_fs_ops_t *ops = fs->ops;
    vfs_unmount(path);

    if (ops == vfs_fat32_get_ops()) {
        fat32_unmount();
        printf("FAT32 unmounted from %s\n", path);
    } else if (ops == vfs_ext4_get_ops()) {
        ext4_unmount();
        printf("ext4 unmounted from %s\n", path);
    } else {
        printf("Filesystem unmounted from %s\n", path);
    }
}

int atoi(const char *str) {
    int result = 0, sign = 1;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str++ - '0');
    }
    return sign * result;
}

void unmount_filesystem(void) {
    if (vfs_has_root_mount()) {
        unmount_filesystem_path("/");
    } else if (fat32_is_mounted()) {
        fat32_unmount();
    } else {
        printf("No filesystem is currently mounted\n");
    }
}

void list_files(void) {
    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' first.\n");
        return;
    }
    printf("Directory listing:\n");
    printf("Type   Size       Name\n");
    printf("-----  ---------- --------\n");

    if (vfs_has_root_mount()) {
        vfs_dirent_t vfs_entries[32];
        int count = vfs_readdir("/", vfs_entries, 32);
        if (count < 0) { printf("Error reading directory\n"); return; }
        if (count == 0) { printf("(empty)\n"); return; }
        for (int i = 0; i < count; i++) {
            printf("%s   %10u   %s\n",
                   (vfs_entries[i].type == VFS_DIRECTORY) ? "DIR " : "FILE",
                   vfs_entries[i].size, vfs_entries[i].name);
        }
        printf("\nTotal: %d items\n", count);
    } else {
        static fat32_dir_list_t entries[32];
        int count = fat32_list_directory("/", entries, 32);
        if (count < 0) { printf("Error reading directory\n"); return; }
        if (count == 0) { printf("(empty)\n"); return; }
        for (int i = 0; i < count; i++) {
            printf("%s   %10d   %s\n",
                   entries[i].is_directory ? "DIR " : "FILE",
                   entries[i].size, entries[i].name);
        }
        printf("\nTotal: %d items\n", count);
    }
}

void filesystem_info(void) {
    if (!vfs_has_root_mount() && !fat32_is_mounted()) { printf("No filesystem mounted\n"); return; }
    printf("Filesystem Information:\n");
    printf("Type: FAT32\n");
    char vol[12];
    memmove(vol, fat32_filesystem.boot_sector.volume_label, 11);
    vol[11] = '\0';
    printf("Volume Label: '%s'\n", vol);
    printf("Bytes/Sector: %d\n", fat32_filesystem.boot_sector.bytes_per_sector);
    printf("Sectors/Cluster: %d\n", fat32_filesystem.boot_sector.sectors_per_cluster);
    printf("Bytes/Cluster: %d\n", fat32_filesystem.bytes_per_cluster);
    printf("Total Sectors: %d\n", fat32_filesystem.boot_sector.total_sectors_32);
    printf("FAT Size: %d sectors\n", fat32_filesystem.boot_sector.fat_size_32);
    printf("Root Dir Cluster: %d\n", fat32_filesystem.root_dir_cluster);
    printf("FAT Start: %d\n", fat32_filesystem.fat_start_sector);
    printf("Data Start: %d\n", fat32_filesystem.data_start_sector);
}

void show_storage_devices(void) {
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;

    printf("Storage Devices:\n");
    printf("ID  Name                     Sectors      Size\n");
    printf("--  ----------------------   ----------   --------\n");

    if (storage_device_count == 0) {
        printf("No storage devices found\n");
        return;
    }

    for (uint8_t i = 0; i < storage_device_count; i++) {
        storage_device_t *dev = &storage_devices[i];
        if (dev->is_initialized) {
            uint64_t size_mb = ((uint64_t)dev->sector_count * dev->sector_size) / (1024 * 1024);
            printf("%u  %s   %u   %llu MB\n",
                   dev->device_id, dev->name, dev->sector_count,
                   (unsigned long long)size_mb);
        }
    }
    printf("\nTotal devices: %d\n", storage_device_count);
}

void show_help(void) {
    printf("Use 'help' in the Zeus shell for available commands.\n");
    printf("Type commands directly - no prefix needed.\n");
}

/* File I/O helpers */

void cat_file(const char *filename) {
    char abs_path[256];
    const char *path = filename;
    if (filename[0] != '/') {
        abs_path[0] = '/';
        int i = 1;
        for (int j = 0; filename[j] && i < 255; j++)
            abs_path[i++] = filename[j];
        abs_path[i] = '\0';
        path = abs_path;
    }
    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' first.\n");
        return;
    }
    if (vfs_has_root_mount()) {
        int vfd = vfs_open(path, VFS_O_READ);
        if (vfd < 0) { printf("Failed to open '%s'\n", filename); return; }
        static char buf[1024];
        int n;
        while ((n = vfs_read(vfd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        printf("\n");
        vfs_close(vfd);
    } else {
        fat32_file_t *file = fat32_open(path, "r");
        if (!file) { printf("Failed to open '%s'\n", filename); return; }
        static char buf[1024];
        int n;
        while ((n = fat32_read(file, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        printf("\n");
        fat32_close(file);
    }
}

void touch_file(const char *filename) {
    if (vfs_has_root_mount())
        vfs_create(filename);
    else
        fat32_create_file(filename);
}

void make_directory(const char *dirname) {
    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' first.\n");
        return;
    }
    int rc = vfs_has_root_mount() ? vfs_mkdir(dirname) : fat32_create_directory(dirname);
    if (rc == 0)
        printf("Created directory '%s'\n", dirname);
    else
        printf("Failed to create directory '%s'\n", dirname);
}

void remove_file(const char *filename) {
    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' first.\n");
        return;
    }
    int rc = vfs_has_root_mount() ? vfs_unlink(filename) : fat32_delete_file(filename);
    if (rc == 0)
        printf("Removed '%s'\n", filename);
    else
        printf("Failed to remove '%s'\n", filename);
}

void copy_file(const char *source, const char *dest) {
    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' first.\n");
        return;
    }
    if (vfs_has_root_mount()) {
        void *buf = NULL;
        int n = vfs_read_file_alloc(source, &buf, 0);
        if (n < 0 || !buf) {
            printf("Failed to read '%s'\n", source);
            return;
        }
        int w = vfs_write_file(dest, buf, (uint32_t)n);
        kfree(buf);
        if (w < 0)
            printf("Failed to write '%s'\n", dest);
        else
            printf("Copied '%s' to '%s' (%d bytes)\n", source, dest, n);
        return;
    }
    fat32_file_t *in = fat32_open(source, "r");
    if (!in) {
        printf("Failed to open '%s'\n", source);
        return;
    }
    if (fat32_create_file(dest) != 0) {
        fat32_close(in);
        printf("Failed to create '%s'\n", dest);
        return;
    }
    fat32_file_t *out = fat32_open(dest, "w");
    if (!out) {
        fat32_close(in);
        printf("Failed to open '%s' for write\n", dest);
        return;
    }
    uint8_t *chunk = kalloc(4096);
    if (!chunk) {
        fat32_close(in);
        fat32_close(out);
        printf("Out of memory\n");
        return;
    }
    int total = 0;
    for (;;) {
        int n = fat32_read(in, chunk, 4096);
        if (n <= 0) break;
        if (fat32_write(out, chunk, (uint32_t)n) != n) {
            printf("Write failed to '%s'\n", dest);
            break;
        }
        total += n;
    }
    kfree(chunk);
    fat32_close(in);
    fat32_close(out);
    printf("Copied '%s' to '%s' (%d bytes)\n", source, dest, total);
}

void move_file(const char *source, const char *dest) {
    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' first.\n");
        return;
    }
    copy_file(source, dest);
    if (vfs_has_root_mount()) {
        if (vfs_unlink(source) != 0)
            printf("Copied but failed to remove '%s'\n", source);
    } else if (fat32_delete_file(source) != 0) {
        printf("Copied but failed to remove '%s'\n", source);
    }
}

/* Network helpers */

void show_network_status(void) {
    printf("Network Status:\n");
    if (!net_is_initialized()) { printf("Network driver not available\n"); return; }

    printf("Driver: %s (Initialized)\n", net_get_driver_name());

    uint8_t mac[6];
    net_get_mac_address(mac);
    printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t ip[4];
    net_get_ip_address(ip);
    printf("IP: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

    if (!ip[0] && !ip[1] && !ip[2] && !ip[3]) {
        printf("\nNOTE: IP not set. Try 'dhcp', 'ddhcp', or 'netset'.\n");
    }

    net_device_t *cur = net_device_get_current();
    if (cur && cur->pci_dev) {
        printf("PCI: Bus %d, Dev %d, Fn %d\n",
               cur->pci_dev->bus, cur->pci_dev->device, cur->pci_dev->function);
    }

    int total, valid;
    arp_get_cache_info(&total, &valid);
    printf("ARP cache: %d total, %d valid\n", total, valid);
}

void show_mac_address(void) {
    if (!net_is_initialized()) { printf("Network not initialized\n"); return; }
    uint8_t mac[6];
    net_get_mac_address(mac);
    printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int parse_ip_address(const char *ip_str, uint8_t *ip_out) {
    int parts[4], count = 0;
    char *str = kalloc(strlen(ip_str) + 1);
    if (!str) return -1;
    kstrncpy(str, ip_str, strlen(ip_str) + 1);
    char *tok = strtok(str, ".");
    while (tok && count < 4) {
        parts[count] = atoi(tok);
        if (parts[count] < 0 || parts[count] > 255) { kfree(str); return -1; }
        count++;
        tok = strtok(NULL, ".");
    }
    kfree(str);
    if (count != 4) return -1;
    for (int i = 0; i < 4; i++) ip_out[i] = (uint8_t)parts[i];
    return 0;
}

int cmd_netset(int argc, char **argv) {
    uint8_t ip[4]={0}, mask[4]={0}, gw[4]={0}, dns[4]={0};
    bool have_ip=false, have_mask=false, have_gw=false, have_dns=false;

    if (argc < 3) return -1;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) break;
        if (kstrcmp(argv[i], "ip") == 0 && parse_ip_address(argv[i+1], ip) == 0) {
            net_set_ip_address(ip); have_ip = true;
        } else if (kstrcmp(argv[i], "mask") == 0 && parse_ip_address(argv[i+1], mask) == 0) {
            net_set_subnet_mask(mask); have_mask = true;
        } else if (kstrcmp(argv[i], "gw") == 0 && parse_ip_address(argv[i+1], gw) == 0) {
            net_set_gateway(gw); have_gw = true;
        } else if (kstrcmp(argv[i], "dns") == 0 && parse_ip_address(argv[i+1], dns) == 0) {
            net_set_dns_server(dns); have_dns = true;
        }
    }

    if (have_ip || have_mask || have_gw) {
        if (!have_ip)   net_get_ip_address(ip);
        if (!have_mask) net_get_subnet_mask(mask);
        if (!have_gw)   net_get_gateway(gw);
        bool gw_set   = (gw[0] | gw[1] | gw[2] | gw[3]) != 0;
        bool mask_set = (mask[0] | mask[1] | mask[2] | mask[3]) != 0;
        if (gw_set && mask_set) {
            bool on_subnet = true;
            for (int i = 0; i < 4; i++) {
                if ((ip[i] & mask[i]) != (gw[i] & mask[i])) { on_subnet = false; break; }
            }
            if (!on_subnet) {
                printf("\nWARNING: Gateway %d.%d.%d.%d unreachable from %d.%d.%d.%d/%d.%d.%d.%d\n",
                       gw[0],gw[1],gw[2],gw[3], ip[0],ip[1],ip[2],ip[3],
                       mask[0],mask[1],mask[2],mask[3]);
            }
        }
    }

    uint8_t tmp[4];
    printf("Net config:\n");
    net_get_ip_address(tmp);
    printf("  IP:   %d.%d.%d.%d%s\n", tmp[0],tmp[1],tmp[2],tmp[3], have_ip?"":" (unchanged)");
    net_get_subnet_mask(tmp);
    printf("  Mask: %d.%d.%d.%d%s\n", tmp[0],tmp[1],tmp[2],tmp[3], have_mask?"":" (unchanged)");
    net_get_gateway(tmp);
    printf("  GW:   %d.%d.%d.%d%s\n", tmp[0],tmp[1],tmp[2],tmp[3], have_gw?"":" (unchanged)");
    net_get_dns_server(tmp);
    printf("  DNS:  %d.%d.%d.%d%s\n", tmp[0],tmp[1],tmp[2],tmp[3], have_dns?"":" (unchanged)");
    return 0;
}

/* USB / input helpers */

void show_usb_status(void) {
    printf("USB Status:\n");
    uint8_t ctrl_count = usb_get_controller_count();
    printf("Controllers: %d\n", ctrl_count);
    if (ctrl_count == 0) { printf("No USB controllers found\n"); return; }

    uint32_t total = 0;
    for (uint8_t i = 0; i < ctrl_count; i++) {
        usb_controller_t *ctrl = usb_get_controller(i);
        if (!ctrl) continue;
        printf("\nController %d: type=0x%02x base=0x%lx devices=%d\n",
               i, ctrl->controller_type, ctrl->base_address, ctrl->device_count);
        usb_device_t *dev = ctrl->devices;
        uint8_t dn = 0;
        while (dev) {
            printf("  Dev %d: addr=%d vid=0x%04x pid=0x%04x class=0x%02x\n",
                   dn++, dev->address, dev->vendor_id, dev->product_id, dev->device_class);
            dev = dev->next;
        }
        total += ctrl->device_count;
    }
    printf("\nTotal USB devices: %d\n", total);
}

void usb_enumerate_devices(void) {
    printf("USB Device Enumeration:\n");
    uint8_t ctrl_count = usb_get_controller_count();
    if (ctrl_count == 0) { printf("No USB controllers found\n"); return; }
    for (uint8_t i = 0; i < ctrl_count; i++) {
        usb_controller_t *ctrl = usb_get_controller(i);
        if (!ctrl) continue;
        printf("Controller %d: ", i);
        if (usb_device_enumeration(ctrl) == 0)
            printf("enumeration OK (%d devices)\n", ctrl->device_count);
        else
            printf("enumeration failed\n");
    }
    printf("Use 'usb' to see details.\n");
}

void usb_keyboard_test(void) {
    printf("USB Keyboard Test:\n");
    uint8_t ctrl_count = usb_get_controller_count();
    if (ctrl_count == 0) { printf("No USB controllers\n"); return; }
    bool found = false;
    for (uint8_t i = 0; i < ctrl_count; i++) {
        usb_controller_t *ctrl = usb_get_controller(i);
        if (!ctrl) continue;
        usb_device_t *dev = ctrl->devices;
        while (dev) {
            if (dev->device_class == USB_CLASS_HID &&
                dev->device_protocol == USB_HID_PROTOCOL_KEYBOARD) {
                printf("Found keyboard: vid=0x%04x pid=0x%04x\n",
                       dev->vendor_id, dev->product_id);
                usb_keyboard_report_t report;
                if (usb_keyboard_read_report(dev, &report) == 0) {
                    printf("Report OK  mod=0x%02x  keys=%02x %02x %02x\n",
                           report.modifier_keys,
                           report.key_codes[0], report.key_codes[1], report.key_codes[2]);
                    usb_keyboard_process_report(&report);
                } else {
                    printf("Report read failed\n");
                }
                found = true;
            }
            dev = dev->next;
        }
    }
    if (!found) printf("No USB keyboards found\n");
    printf("USB keyboard test complete.\n");
}

void cmd_inputinfo(void) {
    printf("Input System Status:\n");
    printf("====================\n");
#ifdef DISABLE_PS2_INPUT
    printf("Mode:   USB-ONLY\n");
    printf("PS/2:   DISABLED\n");
    printf("USB:    ENABLED\n");
#else
    printf("Mode:   HYBRID (PS/2 + USB)\n");
    printf("PS/2:   ENABLED\n");
    printf("USB:    ENABLED\n");
#endif

    printf("\nUSB: %d controller(s)\n", usb_get_controller_count());
    printf("USB keyboard: %s\n", usb_keyboard_has_input() ? "READY" : "WAITING");

#ifndef DISABLE_PS2_INPUT
    printf("PS/2 keyboard: %s\n",
           keyboard_has_echo_pending() ? "ACTIVE" : "IDLE");
    printf("  Buffer len: %d  Execute: %s\n",
           keyboard_get_buffer_length(),
           keyboard_get_execute() ? "SET" : "CLEAR");
    printf("Mouse: %s\n", mouse_is_initialized() ? "initialized" : "not initialized");
    if (mouse_is_initialized()) {
        int mx, my;
        mouse_get_position(&mx, &my);
        printf("  Position: (%d, %d)  Buttons: 0x%02x\n", mx, my, mouse_get_buttons());
    }
#endif
}

void usb_keyboard_poll_test(void) {
    printf("USB Keyboard Polling Test (100 polls, Ctrl+C to abort):\n");
    set_terminate_flag(false);
    for (uint32_t i = 0; i < 100; i++) {
        if (get_terminate_flag()) {
            printf("\nInterrupted\n");
            set_terminate_flag(false);
            return;
        }
        usb_keyboard_poll_all();
        if (usb_keyboard_has_input()) {
            char c = usb_keyboard_get_char();
            if (c) printf("USB key: '%c' (0x%02x)\n", c, (unsigned char)c);
        }
        for (volatile int j = 0; j < 1000000; j++) {}
    }
    printf("USB keyboard polling test complete.\n");
}

/* Image display */

static void display_image(png_image_t *image, uint32_t fb_width, uint32_t fb_height) {
    extern struct limine_framebuffer *fb;
    if (!fb || !image || !image->pixels) {
        printf("Error: Invalid parameters for image display\n");
        return;
    }

    uint32_t dw = (image->width  > fb_width)  ? fb_width  : image->width;
    uint32_t dh = (image->height > fb_height) ? fb_height : image->height;
    uint32_t dx = (fb_width  - dw) / 2;
    uint32_t dy = (fb_height - dh) / 2;

    if (dy + dh > fb_height - 100)
        dy = fb_height - dh - 100;

    printf("Displaying %ux%u image at (%u, %u)\n", dw, dh, dx, dy);

    uint32_t written = 0;
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t px = x < image->width  ? x : image->width  - 1;
            uint32_t py = y < image->height ? y : image->height - 1;
            uint32_t pixel = image->pixels[py * image->width + px];
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >>  8) & 0xFF;
            uint8_t b =  pixel        & 0xFF;
            uint32_t color = (r << 16) | (g << 8) | b;
            uint32_t fx = dx + x, fy = dy + y;
            if (fx >= fb_width || fy >= fb_height) continue;
            if (fb->bpp == 32) {
                volatile uint32_t *screen = (uint32_t *)fb->address;
                uint32_t ppl = fb->pitch / 4;
                screen[(uint64_t)fy * ppl + fx] = color;
            } else {
                putpixel(fx, fy, color);
            }
            written++;
        }
    }
    printf("Image displayed (%u pixels written)\n", written);
}

void cmd_img(const char *filename) {
    extern struct limine_framebuffer *fb;
    if (!fb) { printf("No framebuffer available\n"); return; }

    char abs_path[256];
    const char *path = filename;
    if (filename[0] != '/') {
        abs_path[0] = '/';
        int i = 1;
        for (int j = 0; filename[j] && i < 255; j++)
            abs_path[i++] = filename[j];
        abs_path[i] = '\0';
        path = abs_path;
    }

    uint32_t fw = fb->width, fh = fb->height;
    printf("Loading image '%s' (fb: %ux%u @ %dbpp)\n", path, fw, fh, fb->bpp);

    uint8_t *data = NULL;
    uint32_t rd = 0;
    uint32_t fsize = 0;

    if (vfs_has_root_mount()) {
        int vfd = vfs_open(path, VFS_O_READ);
        if (vfd < 0) { printf("Cannot open '%s'\n", path); goto test_pattern; }
        int end = vfs_seek(vfd, 0, VFS_SEEK_END);
        if (end <= 0) { vfs_close(vfd); printf("Cannot read '%s'\n", path); goto test_pattern; }
        fsize = (uint32_t)end;
        vfs_seek(vfd, 0, VFS_SEEK_SET);
        data = kalloc(fsize);
        if (!data) { vfs_close(vfd); printf("Out of memory\n"); goto test_pattern; }
        rd = (uint32_t)vfs_read(vfd, data, fsize);
        vfs_close(vfd);
    } else {
        fat32_file_t *file = fat32_open(path, "r");
        if (!file) { printf("Cannot open '%s'\n", path); goto test_pattern; }
        fsize = file->file_size;
        data = kalloc(fsize);
        if (!data) { fat32_close(file); printf("Out of memory\n"); goto test_pattern; }
        rd = fat32_read(file, data, fsize);
        fat32_close(file);
    }

    if (rd != fsize) {
        printf("Read error (%u/%u bytes)\n", rd, fsize);
        kfree(data); goto test_pattern;
    }

    simple_img_t *si = simple_img_decode(data, rd);
    png_image_t  *img = NULL;

    if (si && si->valid) {
        img = kalloc(sizeof(png_image_t));
        if (img) {
            img->width = si->width; img->height = si->height;
            img->pixels = si->pixels; img->valid = true;
            si->pixels = NULL;
        }
        simple_img_free(si);
    } else {
        img = png_decode(data, rd);
    }
    kfree(data);

    if (img && img->pixels && img->valid) {
        display_image(img, fw, fh);
        png_free(img);
        return;
    }
    if (img)
        png_free(img);
    printf("Image decode failed, showing test pattern\n");

test_pattern: {
    uint32_t iw = fw < 320 ? fw : 320;
    uint32_t ih = fh < 240 ? fh : 240;
    png_image_t *t = kalloc(sizeof(png_image_t));
    if (!t) return;
    t->width = iw; t->height = ih; t->valid = true;
    t->pixels = kalloc(iw * ih * sizeof(uint32_t));
    if (!t->pixels) { kfree(t); return; }
    uint32_t mx = iw > 1 ? iw - 1 : 1, my = ih > 1 ? ih - 1 : 1;
    for (uint32_t y = 0; y < ih; y++)
        for (uint32_t x = 0; x < iw; x++)
            t->pixels[y * iw + x] = (0xFF << 24) |
                                    ((x * 255 / mx) << 16) |
                                    ((y * 255 / my) <<  8) |
                                    (((x + y) * 128 / (mx + my)));
    display_image(t, fw, fh);
    kfree(t->pixels); kfree(t);
    }
}

/* ================================================================
 * System information (`neofetch`)
 *
 * Moved here from src/usoft/info.c: these helpers read kernel state
 * (page counts, CPU count, storage devices), so they belong to the
 * kernel, not to a pseudo user-space directory.  snprintf() now lives
 * in term.c next to vsnprintf().
 * ================================================================ */

void format_uptime(uint64_t ticks, char* buffer, size_t buffer_size) {
    /* hpet_get_ticks() already returns milliseconds (hpet.c divides the raw
     * main counter by hpet_freq/1000).  The copied-over code divided by
     * hpet_freq instead, so uptime always printed as "0s". */
    uint64_t seconds = ticks / 1000;

    uint64_t days = seconds / 86400;
    uint64_t hours = (seconds % 86400) / 3600;
    uint64_t minutes = (seconds % 3600) / 60;
    uint64_t secs = seconds % 60;

    if (days > 0) {
        snprintf(buffer, buffer_size, "%lud %luh %lum %lus",
                (unsigned long)days, (unsigned long)hours,
                (unsigned long)minutes, (unsigned long)secs);
    } else if (hours > 0) {
        snprintf(buffer, buffer_size, "%luh %lum %lus",
                (unsigned long)hours, (unsigned long)minutes,
                (unsigned long)secs);
    } else if (minutes > 0) {
        snprintf(buffer, buffer_size, "%lum %lus",
                (unsigned long)minutes, (unsigned long)secs);
    } else {
        snprintf(buffer, buffer_size, "%lus", (unsigned long)secs);
    }
}

void format_memory_size(uint64_t bytes, char* buffer, size_t buffer_size) {
    if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        uint64_t unit = (uint64_t)1024 * 1024 * 1024;
        uint64_t whole = bytes / unit;
        uint64_t frac  = (bytes % unit) * 10 / unit;
        snprintf(buffer, buffer_size, "%lu.%lu GiB", (unsigned long)whole, (unsigned long)frac);
    } else if (bytes >= 1024 * 1024) {
        uint64_t unit = 1024 * 1024;
        uint64_t whole = bytes / unit;
        uint64_t frac  = (bytes % unit) * 10 / unit;
        snprintf(buffer, buffer_size, "%lu.%lu MiB", (unsigned long)whole, (unsigned long)frac);
    } else if (bytes >= 1024) {
        uint64_t whole = bytes / 1024;
        uint64_t frac  = (bytes % 1024) * 10 / 1024;
        snprintf(buffer, buffer_size, "%lu.%lu KiB", (unsigned long)whole, (unsigned long)frac);
    } else {
        snprintf(buffer, buffer_size, "%lu B", (unsigned long)bytes);
    }
}

void print_info(void) {
    printf("+--------------------------------------------------------------+\n");
    printf("|                        inDevOS System Info                   |\n");
    printf("+--------------------------------------------------------------+\n");
    printf("\n");

    // OS Information
    printf("OS:           inDevOS 0.0.1 (x86_64)\n");
    printf("Kernel:       Custom x86_64 kernel\n");
    printf("Architecture: x86_64\n");

    // Uptime
    uint64_t ticks = hpet_get_ticks();
    char uptime_str[64];
    format_uptime(ticks, uptime_str, sizeof(uptime_str));
    printf("Uptime:       %s\n", uptime_str);
    printf("HPET Ticks:   %lu\n", (unsigned long)ticks);

    // CPU Information
    char* cpu_name_str = cpu_name();
    if (cpu_name_str) {
        printf("CPU:          %s\n", cpu_name_str);
        kfree(cpu_name_str);
    } else {
        printf("CPU:          Unknown x86_64 processor\n");
    }
    printf("CPU Cores:    %lu\n", (unsigned long)cpu_count);

    // Memory Information
    uint64_t free_bytes  = num_pages_available * (uint64_t)PAGE_SIZE;
    uint64_t used_bytes  = total_memory_bytes > free_bytes ? total_memory_bytes - free_bytes : 0;
    char total_mem_str[32];
    char used_mem_str[32];
    char free_mem_str[32];
    format_memory_size(total_memory_bytes, total_mem_str, sizeof(total_mem_str));
    format_memory_size(used_bytes, used_mem_str, sizeof(used_mem_str));
    format_memory_size(free_bytes, free_mem_str, sizeof(free_mem_str));
    printf("Memory:       %s used / %s total\n", used_mem_str, total_mem_str);
    printf("Free:         %s (%lu pages)\n", free_mem_str, (unsigned long)num_pages_available);
    printf("Allocatable:  %lu pages total\n", (unsigned long)max_pages_available);

    // Framebuffer/Display Information
    uint32_t fb_width = fb_get_width();
    uint32_t fb_height = fb_get_height();
    if (fb_width > 0 && fb_height > 0) {
        printf("Resolution:   %ux%u\n", fb_width, fb_height);
        printf("Console:      %ux%u characters\n", console_get_width(), console_get_height());
    } else {
        printf("Display:      No framebuffer available\n");
    }

    // Storage Information
    printf("Storage:      %d device(s)\n", storage_device_count);
    for (uint8_t i = 0; i < storage_device_count; i++) {
        storage_device_t *dev = &storage_devices[i];
        if (dev->is_initialized) {
            char dev_size_str[32];
            uint64_t dev_size = ((uint64_t)dev->sector_count * dev->sector_size);
            format_memory_size(dev_size, dev_size_str, sizeof(dev_size_str));
            printf("  %s: %s (%u sectors)\n", dev->name, dev_size_str, dev->sector_count);
        }
    }

    // HPET Timer Information
    printf("HPET:         %lu Hz\n", (unsigned long)hpet_get_frequency());
    printf("Timer Mode:   %s\n", hpet_get_current_interval() > 0 ? "Periodic" : "One-shot");

    // GPU Information (if available)
    if (amd_gpu_is_available()) {
        printf("GPU:          AMD GPU detected\n");
    } else {
        printf("GPU:          No dedicated GPU detected\n");
    }

    // System Status
    printf("Status:       Running\n");
    printf("Interrupts:   Enabled\n");

    // Memory Map Summary
    printf("Memory Map:   %lu entries\n", (unsigned long)memmap->entry_count);

    // Bootloader Information (if available)
    printf("Bootloader:   Limine\n");
}

/* ================================================================
 * Command registry / tab completion now live in zeus.c, which owns the
 * shell's command vocabulary (zeus_command_names, zeus_tab_complete).
 * Keeping a second list here caused the two to drift apart.
 * ================================================================ */

