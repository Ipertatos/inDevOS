#include "fat32.h"
#include "utils.h"
#include "mem.h"

fat32_fs_t fat32_filesystem;
fat32_file_t open_files[MAX_OPEN_FILES];

static void fat32_init_file_table() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
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
    if (!fs->fat_table || cluster < 2) {
        return FAT32_CLUSTER_END;
    }
    
    uint32_t fat_offset = cluster;
    if (fat_offset >= (fs->boot_sector.fat_size_32 * fs->boot_sector.bytes_per_sector / 4)) {
        return FAT32_CLUSTER_END;
    }
    
    uint32_t next_cluster = fs->fat_table[fat_offset] & 0x0FFFFFFF;
    
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
    uint32_t start_sector = fat32_cluster_to_sector(fs, cluster);
    if (start_sector == 0) {
        printf("fat32: Invalid cluster {d} - cluster_to_sector returned 0{n}", cluster);
        return -1;
    }
    
    // Debug: printf("fat32: Reading cluster {d} at sector {d}{n}", cluster, start_sector);
    
    int result = storage_read_sectors(fs->storage_dev->device_id, start_sector, 
                                    fs->sectors_per_cluster, buffer);
    if (result != 0) {
        printf("fat32: Storage read failed for sector {d} (result: {d}){n}", start_sector, result);
    }
    
    return result;
}

int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const void *buffer) {
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
    
    for (int i = 0; long_name[i] && name_len < 8; i++) {
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

int fat32_mount(uint8_t storage_device_id) {
    memset(&fat32_filesystem, 0, sizeof(fat32_filesystem));
    fat32_init_file_table();
    
    storage_device_t *dev = storage_get_device(storage_device_id);
    if (!dev) {
        printf("fat32: Storage device not found{n}");
        return -1;
    }
    
    fat32_filesystem.storage_dev = dev;
    
    // Read boot sector
    uint8_t boot_sector_buffer[SECTOR_SIZE];
    if (storage_read_sectors(storage_device_id, 0, 1, boot_sector_buffer) != 0) {
        printf("fat32: Failed to read boot sector{n}");
        return -1;
    }
    
    fat32_boot_sector_t *boot = (fat32_boot_sector_t*)boot_sector_buffer;
    memmove(&fat32_filesystem.boot_sector, boot, sizeof(fat32_boot_sector_t));
    
    // Validate FAT32 signature
    if (boot->boot_signature != FAT32_SIGNATURE) {
        printf("fat32: Invalid boot signature (0x{x}){n}", boot->boot_signature);
        return -1;
    }
    
    // Calculate filesystem layout
    fat32_filesystem.sectors_per_cluster = boot->sectors_per_cluster;
    fat32_filesystem.bytes_per_cluster = boot->bytes_per_sector * boot->sectors_per_cluster;
    fat32_filesystem.fat_start_sector = boot->reserved_sector_count;
    fat32_filesystem.data_start_sector = boot->reserved_sector_count + 
                                       (boot->num_fats * boot->fat_size_32);
    fat32_filesystem.root_dir_cluster = boot->root_cluster;
    
    printf("fat32: Filesystem layout:{n}");
    printf("  Bytes per sector: {d}{n}", boot->bytes_per_sector);
    printf("  Sectors per cluster: {d}{n}", boot->sectors_per_cluster);
    printf("  FAT start sector: {d}{n}", fat32_filesystem.fat_start_sector);
    printf("  Data start sector: {d}{n}", fat32_filesystem.data_start_sector);
    printf("  Root directory cluster: {d}{n}", fat32_filesystem.root_dir_cluster);
    
    // Load FAT table
    uint32_t fat_size_bytes = boot->fat_size_32 * boot->bytes_per_sector;
    fat32_filesystem.fat_table = kalloc(fat_size_bytes);
    if (!fat32_filesystem.fat_table) {
        printf("fat32: Failed to allocate FAT table{n}");
        return -1;
    }
    
    if (storage_read_sectors(storage_device_id, fat32_filesystem.fat_start_sector, 
                           boot->fat_size_32, fat32_filesystem.fat_table) != 0) {
        printf("fat32: Failed to read FAT table{n}");
        kfree(fat32_filesystem.fat_table);
        return -1;
    }
    
    fat32_filesystem.is_mounted = true;
    printf("fat32: Filesystem mounted successfully{n}");
    return 0;
}

void fat32_unmount() {
    if (fat32_filesystem.fat_table) {
        kfree(fat32_filesystem.fat_table);
    }
    memset(&fat32_filesystem, 0, sizeof(fat32_filesystem));
    printf("fat32: Filesystem unmounted{n}");
}

bool fat32_is_mounted() {
    return fat32_filesystem.is_mounted;
}

int fat32_list_directory(const char *path, fat32_dir_list_t *entries, uint32_t max_entries) {
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted{n}");
        return -1;
    }
    
    uint32_t dir_cluster = fat32_filesystem.root_dir_cluster;
    uint32_t entry_count = 0;
    
    // Allocate cluster buffer
    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: Failed to allocate cluster buffer{n}");
        return -1;
    }
    
    while (dir_cluster != FAT32_CLUSTER_END && entry_count < max_entries) {
        // Debug: printf("fat32: Reading directory cluster {d}{n}", dir_cluster);
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            printf("fat32: Failed to read directory cluster {d}{n}", dir_cluster);
            printf("fat32: Cluster-to-sector calculation: {d}{n}", 
                   fat32_cluster_to_sector(&fat32_filesystem, dir_cluster));
            kfree(cluster_buffer);
            return -1;
        }
        
        fat32_dir_entry_t *dir_entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (uint32_t i = 0; i < entries_per_cluster && entry_count < max_entries; i++) {
            fat32_dir_entry_t *entry = &dir_entries[i];
            
            // Skip empty entries
            if (entry->name[0] == 0x00) {
                break; // End of directory
            }
            if (entry->name[0] == 0xE5) {
                continue; // Deleted entry
            }
            
            // Skip LFN entries for now
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                continue;
            }
            
            // Skip volume label
            if (entry->attr & FAT32_ATTR_VOLUME_ID) {
                continue;
            }
            
            // Format filename
            fat32_format_83_filename(entry->name, entries[entry_count].name);
            entries[entry_count].size = entry->file_size;
            entries[entry_count].attributes = entry->attr;
            entries[entry_count].is_directory = (entry->attr & FAT32_ATTR_DIRECTORY) != 0;
            
            entry_count++;
        }
        
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }
    
    kfree(cluster_buffer);
    return entry_count;
}

fat32_file_t* fat32_open(const char *filename, const char *mode) {
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted{n}");
        return NULL;
    }
    
    // Find free file handle
    fat32_file_t *file = NULL;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            file = &open_files[i];
            break;
        }
    }
    
    if (!file) {
        printf("fat32: No free file handles{n}");
        return NULL;
    }
    
    // Simple implementation: search root directory only
    fat32_dir_list_t entries[64];
    int entry_count = fat32_list_directory("/", entries, 64);
    
    for (int i = 0; i < entry_count; i++) {
        if (strEql(entries[i].name, filename)) {
            // Found the file
            file->in_use = true;
            file->file_size = entries[i].size;
            file->file_position = 0;
            file->attributes = entries[i].attributes;
            file->filesystem = &fat32_filesystem;
            memmove(file->filename, filename, strlen(filename) + 1);
            
            // Find first cluster (simplified - would need to read directory entry properly)
            file->first_cluster = 2; // Placeholder
            file->current_cluster = file->first_cluster;
            
            printf("fat32: Opened file '{s}' (size: {d} bytes){n}", filename, file->file_size);
            return file;
        }
    }
    
    printf("fat32: File '{s}' not found{n}", filename);
    return NULL;
}

int fat32_close(fat32_file_t *file) {
    if (!file || !file->in_use) {
        return -1;
    }
    
    file->in_use = false;
    printf("fat32: Closed file '{s}'{n}", file->filename);
    return 0;
}

int fat32_read(fat32_file_t *file, void *buffer, uint32_t size) {
    if (!file || !file->in_use) {
        return -1;
    }
    
    printf("fat32: Read operation not fully implemented yet{n}");
    return 0; // Placeholder
}

int fat32_write(fat32_file_t *file, const void *buffer, uint32_t size) {
    if (!file || !file->in_use) {
        return -1;
    }
    
    printf("fat32: Write operation not fully implemented yet{n}");
    return 0; // Placeholder
}

int fat32_seek(fat32_file_t *file, uint32_t position) {
    if (!file || !file->in_use) {
        return -1;
    }
    
    file->file_position = position;
    return 0;
} 