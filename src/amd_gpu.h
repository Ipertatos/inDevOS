#ifndef AMD_GPU_H
#define AMD_GPU_H

#include "types.h"

void amd_gpu_init(void);

// GPU drawing functions
void amd_gpu_fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void amd_gpu_clear_screen(uint32_t color);
bool amd_gpu_is_available(void);
void amd_gpu_get_info(uint32_t *width, uint32_t *height);

// AtomBIOS Common Header
typedef struct {
    uint16_t structure_size;
    uint8_t table_format_revision;
    uint8_t table_content_revision;
} atom_common_table_header_t;

// Atom ROM Header
typedef struct {
    atom_common_table_header_t common_header;
    char atom_bios_string[4];  // "ATOM"
    uint16_t bios_runtime_seg_addr;
    uint16_t protected_mode_info_offset;
    uint16_t config_filename_offset;
    uint16_t crc_block_offset;
    uint16_t bios_identifier_offset;
    uint16_t int10_offset;
    uint16_t pci_busdev_init_code;
    uint16_t io_base_addr;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint16_t pci_info_offset;
    uint16_t master_command_table_offset;  // Offset for Command Table
    uint16_t master_data_table_offset;     // Offset for Data Table
    uint8_t extended_function_code;
    uint8_t reserved;
} atom_rom_header_t;

// ... can add more structures later

#endif 