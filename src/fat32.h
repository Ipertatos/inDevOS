#pragma once

#include "types.h"
#include "storage.h"

#define FAT32_SIGNATURE 0x29
#define FAT32_CLUSTER_END 0x0FFFFFFF
#define FAT32_CLUSTER_BAD 0x0FFFFFF7
#define FAT32_CLUSTER_FREE 0x00000000
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

#define MAX_FILENAME_LEN 255
#define MAX_PATH_LEN 1024
#define MAX_OPEN_FILES 32

// FAT32 Boot Sector structure
typedef struct __attribute__((packed)) {
    uint8_t jump_boot[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    
    // FAT32 specific fields
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved_0[12];
    uint8_t drive_number;
    uint8_t reserved_1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} fat32_boot_sector_t;

// FAT32 Directory Entry
typedef struct __attribute__((packed)) {
    char name[11];
    uint8_t attr;
    uint8_t nt_reserved;
    uint8_t created_time_tenth;
    uint16_t created_time;
    uint16_t created_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat32_dir_entry_t;

// FAT32 Long File Name Entry
typedef struct __attribute__((packed)) {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_lo;
    uint16_t name3[2];
} fat32_lfn_entry_t;

// FAT32 Filesystem structure
typedef struct {
    storage_device_t *storage_dev;
    fat32_boot_sector_t boot_sector;
    uint32_t fat_start_sector;
    uint32_t data_start_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t root_dir_cluster;
    uint32_t *fat_table;
    bool is_mounted;
} fat32_fs_t;

// File handle structure
typedef struct {
    bool in_use;
    uint32_t current_cluster;
    uint32_t file_size;
    uint32_t file_position;
    uint32_t first_cluster;
    char filename[MAX_FILENAME_LEN];
    uint8_t attributes;
    fat32_fs_t *filesystem;
} fat32_file_t;

// Directory listing structure
typedef struct {
    char name[MAX_FILENAME_LEN];
    uint32_t size;
    uint8_t attributes;
    bool is_directory;
} fat32_dir_list_t;

extern fat32_fs_t fat32_filesystem;
extern fat32_file_t open_files[MAX_OPEN_FILES];

// Function declarations
int fat32_mount(uint8_t storage_device_id);
void fat32_unmount();
bool fat32_is_mounted();

// File operations
fat32_file_t* fat32_open(const char *filename, const char *mode);
int fat32_close(fat32_file_t *file);
int fat32_read(fat32_file_t *file, void *buffer, uint32_t size);
int fat32_write(fat32_file_t *file, const void *buffer, uint32_t size);
int fat32_seek(fat32_file_t *file, uint32_t position);

// Directory operations
int fat32_list_directory(const char *path, fat32_dir_list_t *entries, uint32_t max_entries);
int fat32_create_directory(const char *dirname);
int fat32_delete_file(const char *filename);
int fat32_create_file(const char *filename);
int fat32_file_exists(const char *filename);

// Utility functions
uint32_t fat32_cluster_to_sector(fat32_fs_t *fs, uint32_t cluster);
uint32_t fat32_get_next_cluster(fat32_fs_t *fs, uint32_t cluster);
int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, void *buffer);
int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const void *buffer);
void fat32_parse_83_filename(const char *long_name, char *short_name);
void fat32_format_83_filename(const char *short_name, char *formatted_name); 