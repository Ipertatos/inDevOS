#pragma once

#include "types.h"
#include "pci.h"

#define SECTOR_SIZE 512
#define MAX_STORAGE_DEVICES 8

typedef struct storage_device {
    uint8_t device_id;
    uint32_t sector_count;
    uint32_t sector_size;
    bool is_initialized;
    struct pci_device *pci_dev;
    uint64_t base_address;
    char name[32];
    bool optical; /* ATAPI CD/DVD. Never an install target. */
} storage_device_t;

// Storage device operations
typedef struct {
    int (*read_sectors)(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
    int (*write_sectors)(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
    int (*flush)(storage_device_t *dev);
} storage_ops_t;

extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
extern uint8_t storage_device_count;
extern storage_ops_t storage_ops;

// Function declarations
void storage_init();
int storage_find_boot_device(void);
storage_device_t* storage_get_device(uint8_t device_id);
int storage_read_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, void *buffer);
int storage_write_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, const void *buffer);
int storage_flush(uint8_t device_id);

// Simple block device emulation (using memory for now)
void storage_init_ramdisk();

// Block cache statistics
void storage_cache_stats(void);
void storage_bcache_invalidate_all(void);
int storage_selftest(uint8_t device_id, uint64_t base);