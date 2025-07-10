#include "fat32.h"
#include "utils.h"
#include "mem.h"
#include "debug.h"
#include "string.h"

// Forward declarations for helper functions used before their definitions
static uint32_t fat32_allocate_cluster(fat32_fs_t *fs);

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

int fat32_mount(uint8_t storage_device_id) {
    DEBUG_ENTER_ARGS("device_id=%d", storage_device_id);
    memset(&fat32_filesystem, 0, sizeof(fat32_filesystem));
    fat32_init_file_table();
    
    storage_device_t *dev = storage_get_device(storage_device_id);
    if (!dev) {
        printf("fat32: Storage device not found\n");
        return -1;
    }
    fat32_filesystem.storage_dev = dev;
    if (!fat32_filesystem.storage_dev) {
        printf("fat32: storage_dev is NULL after assignment!\n");
        return -1;
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
        return -1;
    }
    fat32_boot_sector_t *boot = (fat32_boot_sector_t*)boot_sector_buffer;
    if (!boot) {
        printf("fat32: boot pointer is NULL!\n");
        return -1;
    }
    memmove(&fat32_filesystem.boot_sector, boot, sizeof(fat32_boot_sector_t));
    // Validate FAT32 signature
    if (boot->boot_signature != FAT32_SIGNATURE) {
        printf("fat32: Invalid boot signature (0x%x) at LBA %u. This device may not be formatted as FAT32. Please format the drive as FAT32 and try again.\n", boot->boot_signature, partition_lba);
        return -1;
    }
    // Calculate filesystem layout
    fat32_filesystem.sectors_per_cluster = boot->sectors_per_cluster;
    fat32_filesystem.bytes_per_cluster = boot->bytes_per_sector * boot->sectors_per_cluster;
    fat32_filesystem.fat_start_sector = boot->reserved_sector_count;
    fat32_filesystem.data_start_sector = boot->reserved_sector_count + 
                                       (boot->num_fats * boot->fat_size_32);
    fat32_filesystem.root_dir_cluster = boot->root_cluster;
    printf("fat32: Filesystem layout:\n");
    printf("  Bytes per sector: %d\n", boot->bytes_per_sector);
    printf("  Sectors per cluster: %d\n", boot->sectors_per_cluster);
    printf("  FAT start sector: %d\n", fat32_filesystem.fat_start_sector);
    printf("  Data start sector: %d\n", fat32_filesystem.data_start_sector);
    printf("  Root directory cluster: %d\n", fat32_filesystem.root_dir_cluster);
    // Load FAT table
    uint32_t fat_size_bytes = boot->fat_size_32 * boot->bytes_per_sector;
    printf("fat32: Allocating FAT table - %d sectors, %d bytes\n", boot->fat_size_32, fat_size_bytes);
    fat32_filesystem.fat_table = kalloc(fat_size_bytes);
    if (!fat32_filesystem.fat_table) {
        printf("fat32: Failed to allocate FAT table\n");
        return -1;
    }
    printf("fat32: FAT table allocated successfully\n");
    printf("fat32: Reading FAT sectors - start: %d, count: %d\n", 
           fat32_filesystem.fat_start_sector, boot->fat_size_32);
    if (storage_read_sectors(storage_device_id, fat32_filesystem.fat_start_sector, 
                           boot->fat_size_32, fat32_filesystem.fat_table) != 0) {
        printf("fat32: Failed to read FAT table\n");
        kfree(fat32_filesystem.fat_table);
        return -1;
    }
    printf("fat32: FAT table read successfully\n");
    fat32_filesystem.is_mounted = true;
    printf("fat32: Filesystem mounted successfully\n");
    return 0;
}

void fat32_unmount() {
    if (fat32_filesystem.fat_table) {
        kfree(fat32_filesystem.fat_table);
    }
    memset(&fat32_filesystem, 0, sizeof(fat32_filesystem));
    printf("fat32: Filesystem unmounted\n");
}

bool fat32_is_mounted() {
    return fat32_filesystem.is_mounted;
}

int fat32_list_directory(const char *path, fat32_dir_list_t *entries, uint32_t max_entries) {
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        return -1;
    }
    if (!entries) {
        printf("fat32_list_directory: entries pointer is NULL!\n");
        return -1;
    }
    uint32_t dir_cluster = fat32_filesystem.root_dir_cluster;
    uint32_t entry_count = 0;
    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: Failed to allocate cluster buffer\n");
        return -1;
    }
    while (dir_cluster != FAT32_CLUSTER_END && entry_count < max_entries) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            printf("fat32: Failed to read directory cluster %d\n", dir_cluster);
            printf("fat32: Cluster-to-sector calculation: %d\n", 
                   fat32_cluster_to_sector(&fat32_filesystem, dir_cluster));
            kfree(cluster_buffer);
            return -1;
        }
        fat32_dir_entry_t *dir_entries = (fat32_dir_entry_t*)cluster_buffer;
        if (!dir_entries) {
            printf("fat32_list_directory: dir_entries is NULL!\n");
            kfree(cluster_buffer);
            return -1;
        }
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);
        for (uint32_t i = 0; i < entries_per_cluster && entry_count < max_entries; i++) {
            fat32_dir_entry_t *entry = &dir_entries[i];
            if (!entry) {
                printf("fat32_list_directory: entry pointer is NULL!\n");
                continue;
            }
            // Skip empty entries (0x00 marks free/never-used)
            if (((uint8_t)entry->name[0]) == 0x00) {
                break; // End of directory
            }
            if (((uint8_t)entry->name[0]) == 0xE5) {
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
            
            // Format filename (apply correct case flags)
            fat32_format_83_filename_display(entry->name, entry->nt_reserved, entries[entry_count].name);
            entries[entry_count].name[MAX_FILENAME_LEN-1] = '\0'; // Ensure null-termination
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
    DEBUG_ENTER_ARGS("filename='%s', mode='%s'", filename, mode);
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        DEBUG_POINT("Filesystem not mounted - returning NULL");
        return NULL;
    }
    
    // Convert user filename to upper-case for case-insensitive compare
    char filename_uc[256];
    size_t fn_len = strlen(filename);
    if(fn_len >= sizeof(filename_uc)) fn_len = sizeof(filename_uc)-1;
    for(size_t i=0;i<fn_len;i++){
        char c = filename[i];
        if(c>='a' && c<='z') c = c - 'a' + 'A';
        filename_uc[i]=c;
    }
    filename_uc[fn_len]='\0';
    
    // Find free file handle
    fat32_file_t *file = NULL;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            file = &open_files[i];
            break;
        }
    }
    
    if (!file) {
        printf("fat32: No free file handles\n");
        return NULL;
    }
    
    // Search for file in root directory with proper cluster extraction
    uint32_t dir_cluster = fat32_filesystem.root_dir_cluster;
    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: Failed to allocate cluster buffer\n");
        return NULL;
    }
    
    while (dir_cluster != FAT32_CLUSTER_END) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return NULL;
        }
        
        fat32_dir_entry_t *dir_entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *entry = &dir_entries[i];
            
            if (((uint8_t)entry->name[0]) == 0x00) break; // End of directory
            if (((uint8_t)entry->name[0]) == 0xE5) continue; // Deleted entry
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) continue; // LFN entry
            if (entry->attr & FAT32_ATTR_VOLUME_ID) continue; // Volume label
            
            static char formatted_name[256];  // Static to avoid stack overflow
            fat32_format_83_filename(entry->name, formatted_name);
            
            if (strEql(formatted_name, filename_uc)) {
                // Found the file
                file->in_use = true;
                file->file_size = entry->file_size;
                file->file_position = 0;
                file->attributes = entry->attr;
                file->filesystem = &fat32_filesystem;
                memmove(file->filename, filename, strlen(filename) + 1);
                
                // Extract first cluster from directory entry
                file->first_cluster = ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
                file->current_cluster = file->first_cluster;
                
                kfree(cluster_buffer);
                        printf("fat32: Opened file '%s' (size: %d bytes, cluster: %d)\n",
               filename, file->file_size, file->first_cluster);
                return file;
            }
        }
        
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }
    
    kfree(cluster_buffer);
    printf("fat32: File '%s' not found\n", filename);
    return NULL;
}

int fat32_close(fat32_file_t *file) {
    if (!file || !file->in_use) {
        return -1;
    }
    
    file->in_use = false;
    printf("fat32: Closed file '%s'\n", file->filename);
    return 0;
}

int fat32_read(fat32_file_t *file, void *buffer, uint32_t size) {
    if (!file || !file->in_use) {
        return -1;
    }
    
    if (file->file_position >= file->file_size) {
        return 0; // EOF
    }
    
    // Limit read size to remaining bytes in file
    uint32_t remaining_bytes = file->file_size - file->file_position;
    if (size > remaining_bytes) {
        size = remaining_bytes;
    }
    
    uint32_t bytes_read = 0;
    uint8_t *output_buffer = (uint8_t*)buffer;
    
    // Allocate cluster buffer
    void *cluster_buffer = kalloc(file->filesystem->bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: Failed to allocate cluster buffer for read\n");
        return -1;
    }
    
    while (bytes_read < size && file->current_cluster != FAT32_CLUSTER_END) {
        // Read current cluster
        if (fat32_read_cluster(file->filesystem, file->current_cluster, cluster_buffer) != 0) {
            printf("fat32: Failed to read cluster %d\n", file->current_cluster);
            kfree(cluster_buffer);
            return -1;
        }
        
        // Calculate position within cluster
        uint32_t cluster_offset = file->file_position % file->filesystem->bytes_per_cluster;
        uint32_t bytes_in_cluster = file->filesystem->bytes_per_cluster - cluster_offset;
        uint32_t bytes_to_copy = size - bytes_read;
        
        if (bytes_to_copy > bytes_in_cluster) {
            bytes_to_copy = bytes_in_cluster;
        }
        
        // Copy data from cluster buffer to output buffer
        memmove(output_buffer + bytes_read, 
                (uint8_t*)cluster_buffer + cluster_offset, 
                bytes_to_copy);
        
        bytes_read += bytes_to_copy;
        file->file_position += bytes_to_copy;
        
        // Move to next cluster if we've read the entire current cluster
        if (cluster_offset + bytes_to_copy >= file->filesystem->bytes_per_cluster) {
            file->current_cluster = fat32_get_next_cluster(file->filesystem, file->current_cluster);
        }
    }
    
    kfree(cluster_buffer);
    return bytes_read;
}

int fat32_write(fat32_file_t *file, const void *buffer, uint32_t size) {
    serial_printf("[fat32_write] file='%s' size=%d first_cluster=%d\n", file ? file->filename : "(null)", (int)size, file ? (int)file->first_cluster : 0);
    // NEW IMPLEMENTATION: basic support for writing up to one cluster (overwrites the file)
    if (!file || !file->in_use) {
        return -1;
    }

    fat32_fs_t *fs = file->filesystem;
    uint32_t bytes_per_cluster = fs->bytes_per_cluster;
    if (size == 0) {
        return 0;
    }

    // Limit write size to one cluster for now
    if (size > bytes_per_cluster) {
        printf("fat32: write size exceeds 1 cluster – truncating to %u bytes\n", bytes_per_cluster);
        size = bytes_per_cluster;
    }

    // Allocate a cluster if the file has none yet
    if (file->first_cluster == 0) {
        uint32_t new_cluster = fat32_allocate_cluster(fs);
        if (new_cluster == FAT32_CLUSTER_END) {
            printf("fat32: No free clusters for write\n");
            return -1;
        }
        file->first_cluster   = new_cluster;
        file->current_cluster = new_cluster;
    }

    // Prepare buffer to fill the cluster
    void *cluster_buf = kalloc(bytes_per_cluster);
    if (!cluster_buf) {
        printf("fat32: Failed to allocate buffer for write\n");
        return -1;
    }
    memset(cluster_buf, 0, bytes_per_cluster);
    memmove(cluster_buf, buffer, size);

    // Write the cluster to disk
    if (fat32_write_cluster(fs, file->first_cluster, cluster_buf) != 0) {
        kfree(cluster_buf);
        printf("fat32: Failed to write cluster %u\n", file->first_cluster);
        return -1;
    }
    kfree(cluster_buf);

    // Update directory entry with cluster info and file size
    void *dir_buf = kalloc(fs->bytes_per_cluster);
    if (!dir_buf) {
        return -1;
    }

    uint32_t dir_cluster = fs->root_dir_cluster;
    // Build an upper-case version of the filename for matching with directory entries
    char filename_uc[256];
    size_t fn_len2 = strlen(file->filename);
    if (fn_len2 >= sizeof(filename_uc)) fn_len2 = sizeof(filename_uc) - 1;
    for (size_t i_uc = 0; i_uc < fn_len2; i_uc++) {
        char c_uc = file->filename[i_uc];
        if (c_uc >= 'a' && c_uc <= 'z') {
            c_uc = c_uc - 'a' + 'A';
        }
        filename_uc[i_uc] = c_uc;
    }
    filename_uc[fn_len2] = '\0';

    while (dir_cluster != FAT32_CLUSTER_END) {
        if (fat32_read_cluster(fs, dir_cluster, dir_buf) != 0) {
            kfree(dir_buf);
            return -1;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)dir_buf;
        uint32_t entries_per_cluster = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *entry = &entries[i];
            if (((uint8_t)entry->name[0]) == 0x00) {
                break; // End of directory
            }
            if (((uint8_t)entry->name[0]) == 0xE5 || (entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                continue; // Deleted or LFN entry
            }

            char formatted[256];
            fat32_format_83_filename(entry->name, formatted);
            if (strEql(formatted, filename_uc)) {
                entry->first_cluster_hi = (uint16_t)(file->first_cluster >> 16);
                entry->first_cluster_lo = (uint16_t)(file->first_cluster & 0xFFFF);
                entry->file_size        = size;

                if (fat32_write_cluster(fs, dir_cluster, dir_buf) != 0) {
                    kfree(dir_buf);
                    return -1;
                }
                kfree(dir_buf);

                file->file_size     = size;
                file->file_position = size;
                serial_printf("[fat32_write] write complete size=%d bytes\n", (int)size);
                return size;
            }
        }
        dir_cluster = fat32_get_next_cluster(fs, dir_cluster);
    }

    kfree(dir_buf);
    printf("fat32: Failed to update directory entry for '%s'\n", file->filename);
    return -1;
}

int fat32_seek(fat32_file_t *file, uint32_t position) {
    if (!file || !file->in_use) {
        return -1;
    }
    
    file->file_position = position;
    return 0;
}

// Helper: flush the in-memory FAT table back to disk (only first FAT copy)
static int fat32_flush_fat(fat32_fs_t *fs) {
    if (!fs || !fs->fat_table || !fs->is_mounted) {
        return -1;
    }
    return storage_write_sectors(fs->storage_dev->device_id,
                                 fs->fat_start_sector,
                                 fs->boot_sector.fat_size_32,
                                 fs->fat_table);
}

// Helper: allocate a single free cluster and mark it as end-of-chain
static uint32_t fat32_allocate_cluster(fat32_fs_t *fs) {
    if (!fs || !fs->fat_table) {
        return FAT32_CLUSTER_END;
    }

    uint32_t total_clusters = (fs->boot_sector.fat_size_32 * fs->boot_sector.bytes_per_sector) / 4;
    // Cluster indexes start at 2
    for (uint32_t cluster = 2; cluster < total_clusters; cluster++) {
        if ((fs->fat_table[cluster] & 0x0FFFFFFF) == FAT32_CLUSTER_FREE) {
            fs->fat_table[cluster] = FAT32_CLUSTER_END; // mark allocated
            fat32_flush_fat(fs);
            return cluster;
        }
    }
    return FAT32_CLUSTER_END; // No space
}

// Helper: free an entire cluster chain (recursively clears FAT entries)
static void fat32_free_cluster_chain(fat32_fs_t *fs, uint32_t start_cluster) {
    if (!fs || !fs->fat_table || start_cluster < 2 || start_cluster == FAT32_CLUSTER_END) {
        return;
    }
    uint32_t cur = start_cluster;
    while (cur != FAT32_CLUSTER_END && cur >= 2) {
        uint32_t next = fs->fat_table[cur] & 0x0FFFFFFF;
        fs->fat_table[cur] = FAT32_CLUSTER_FREE;
        if (next == FAT32_CLUSTER_END || next == FAT32_CLUSTER_BAD || next == FAT32_CLUSTER_FREE) {
            break;
        }
        cur = next;
    }
    fat32_flush_fat(fs);
}

int fat32_create_file(const char *filename) {
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        return -1;
    }

    // Convert to 8.3 short name
    char short_name[11];
    fat32_parse_83_filename(filename, short_name);

    // Buffer for directory cluster
    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: create_file – failed to allocate buffer\n");
        return -1;
    }

    uint32_t dir_cluster = fat32_filesystem.root_dir_cluster;
    while (dir_cluster != FAT32_CLUSTER_END) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return -1;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *entry = &entries[i];

            // Free entry? 0x00 means never used, 0xE5 means deleted
            if (((uint8_t)entry->name[0]) == 0x00 || (((uint8_t)entry->name[0]) == 0xE5)) {
                // Populate directory entry
                memmove(entry->name, short_name, 11);
                entry->attr = FAT32_ATTR_ARCHIVE; // regular file
                entry->nt_reserved = 0;
                entry->created_time_tenth = 0;
                entry->created_time = 0;
                entry->created_date = 0;
                entry->last_access_date = 0;
                entry->write_time = 0;
                entry->write_date = 0;
                entry->first_cluster_hi = 0;
                entry->first_cluster_lo = 0;
                entry->file_size = 0;

                // Write back cluster
                if (fat32_write_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
                    kfree(cluster_buffer);
                    return -1;
                }
                kfree(cluster_buffer);
                printf("fat32: Created empty file '%s'\n", filename);
                return 0;
            }
        }

        // Move to next directory cluster
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }

    kfree(cluster_buffer);
    printf("fat32: Directory is full – unable to create '%s'\n", filename);
    return -1;
}

// Create a directory in the root (minimal implementation)
int fat32_create_directory(const char *dirname) {
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        return -1;
    }

    char short_name[11];
    fat32_parse_83_filename(dirname, short_name);

    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: create_directory – failed to allocate buffer\n");
        return -1;
    }

    uint32_t dir_cluster = fat32_filesystem.root_dir_cluster;
    while (dir_cluster != FAT32_CLUSTER_END) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return -1;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *entry = &entries[i];
            if (((uint8_t)entry->name[0]) == 0x00 || (((uint8_t)entry->name[0]) == 0xE5)) {
                // Allocate a single cluster for the new directory (optional)
                uint32_t new_cluster = fat32_allocate_cluster(&fat32_filesystem);
                if (new_cluster == FAT32_CLUSTER_END) {
                    kfree(cluster_buffer);
                    printf("fat32: No space to allocate directory cluster\n");
                    return -1;
                }

                // Zero out the new directory cluster so it starts empty
                void *new_dir_buf = kalloc(fat32_filesystem.bytes_per_cluster);
                if (!new_dir_buf) {
                    kfree(cluster_buffer);
                    return -1;
                }
                memset(new_dir_buf, 0, fat32_filesystem.bytes_per_cluster);
                fat32_write_cluster(&fat32_filesystem, new_cluster, new_dir_buf);
                kfree(new_dir_buf);

                // Populate directory entry
                memmove(entry->name, short_name, 11);
                entry->attr = FAT32_ATTR_DIRECTORY;
                entry->nt_reserved = 0;
                entry->created_time_tenth = 0;
                entry->created_time = 0;
                entry->created_date = 0;
                entry->last_access_date = 0;
                entry->write_time = 0;
                entry->write_date = 0;
                entry->first_cluster_hi = (uint16_t)(new_cluster >> 16);
                entry->first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
                entry->file_size = 0;

                if (fat32_write_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
                    kfree(cluster_buffer);
                    return -1;
                }
                kfree(cluster_buffer);
                printf("fat32: Created directory '%s'\n", dirname);
                return 0;
            }
        }
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }
    kfree(cluster_buffer);
    printf("fat32: Directory table full – unable to create directory '%s'\n", dirname);
    return -1;
}

int fat32_delete_file(const char *filename) {
    if (!fat32_is_mounted()) {
        printf("fat32: Filesystem not mounted\n");
        return -1;
    }

    char short_name[11];
    fat32_parse_83_filename(filename, short_name);

    void *cluster_buffer = kalloc(fat32_filesystem.bytes_per_cluster);
    if (!cluster_buffer) {
        printf("fat32: delete_file – failed to allocate buffer\n");
        return -1;
    }

    uint32_t dir_cluster = fat32_filesystem.root_dir_cluster;
    while (dir_cluster != FAT32_CLUSTER_END) {
        if (fat32_read_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return -1;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buffer;
        uint32_t entries_per_cluster = fat32_filesystem.bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t *entry = &entries[i];
            if (((uint8_t)entry->name[0]) == 0x00) {
                break; // End of directory, not found
            }
            if (((uint8_t)entry->name[0]) == 0xE5) {
                continue; // Deleted
            }
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                continue; // Skip LFN
            }

            if (memcmp(entry->name, short_name, 11) == 0) {
                // Found entry – free cluster chain if any
                uint32_t first_cluster = ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
                if (first_cluster >= 2) {
                    fat32_free_cluster_chain(&fat32_filesystem, first_cluster);
                }

                // Mark entry as deleted
                entry->name[0] = 0xE5;
                if (fat32_write_cluster(&fat32_filesystem, dir_cluster, cluster_buffer) != 0) {
                    kfree(cluster_buffer);
                    return -1;
                }
                kfree(cluster_buffer);
                printf("fat32: Deleted file '%s'\n", filename);
                return 0;
            }
        }
        dir_cluster = fat32_get_next_cluster(&fat32_filesystem, dir_cluster);
    }

    kfree(cluster_buffer);
    printf("fat32: File '%s' not found for deletion\n", filename);
    return -1;
}

// Get file information
int fat32_file_exists(const char *filename) {
    if (!fat32_is_mounted()) {
        return 0;
    }
    
    fat32_dir_list_t entries[64];
    int entry_count = fat32_list_directory("/", entries, 64);
    
    for (int i = 0; i < entry_count; i++) {
        if (strEql(entries[i].name, filename)) {
            return 1; // File exists
        }
    }
    return 0; // File doesn't exist
} 