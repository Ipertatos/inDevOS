#pragma once

#include "types.h"
#include "driver_manager.h"

// Driver file format magic number
#define DRIVER_FILE_MAGIC 0x44524956  // "DRIV"

// Driver file format version
#define DRIVER_FILE_VERSION 0x0100

// Driver file header structure
typedef struct {
    uint32_t magic;           // Magic number (DRIV)
    uint16_t version;         // File format version
    uint16_t header_size;     // Size of this header
    uint32_t driver_size;     // Size of driver data
    uint32_t checksum;        // Simple checksum of driver data
    char name[64];           // Driver name
    char description[256];   // Driver description
    uint32_t flags;          // Driver flags
} driver_file_header_t;

// Driver file flags
#define DRIVER_FLAG_REQUIRES_INTERRUPTS 0x00000001
#define DRIVER_FLAG_SUPPORTS_HOTPLUG    0x00000002
#define DRIVER_FLAG_KERNEL_DRIVER       0x00000004
#define DRIVER_FLAG_USER_DRIVER         0x00000008

// Driver file functions
int driver_file_write(const char *filename, const driver_registration_t *driver);
int driver_file_read(const char *filename, driver_registration_t *driver);
uint32_t driver_file_calculate_checksum(const void *data, uint32_t size);
bool driver_file_validate_header(const driver_file_header_t *header); 