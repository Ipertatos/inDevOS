#include "amd_gpu.h"
#include "pci.h"
#include "serial.h"
#include "types.h"
#include "stdio.h"
#include "mem.h"

extern int atom_execute(void* rom, uint16_t cmd_offset, void* params);

// Global AMD GPU state
static struct pci_device *amdgpu_device = NULL;
static void* mmio_base = NULL;
static void* vram_base = NULL;
static uint32_t gpu_width = 0;
static uint32_t gpu_height = 0;
static bool gpu_initialized = false;

// AMD GPU drawing functions
bool amd_gpu_is_available(void) {
#if !ENABLE_AMD_GPU
    return false;
#endif
    return gpu_initialized && amdgpu_device != NULL;
}

void amd_gpu_get_info(uint32_t *width, uint32_t *height) {
    if (width) *width = gpu_width;
    if (height) *height = gpu_height;
}

void amd_gpu_clear_screen(uint32_t color) {
    if (!amd_gpu_is_available()) return;
    amd_gpu_fillrect(color, 0, 0, gpu_width, gpu_height);
}

void amd_gpu_fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!amd_gpu_is_available()) {
        printf("AMD GPU: Driver not available, falling back to direct framebuffer\n");
        // Fallback to direct framebuffer access
        extern void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
        fillrect(color, x, y, w, h);
        return;
    }

    printf("AMD GPU: Drawing rectangle via AMD GPU driver\n");
    printf("AMD GPU: Using MMIO at %p for hardware-accelerated drawing\n", mmio_base);
    
    // Bounds checking
    if (x >= gpu_width || y >= gpu_height) return;
    if (x + w > gpu_width) w = gpu_width - x;
    if (y + h > gpu_height) h = gpu_height - y;

    // For demonstration, we'll use both MMIO register access and direct VRAM
    if (mmio_base) {
        // Write to AMD GPU command registers (example registers)
        volatile uint32_t* cmd_regs = (volatile uint32_t*)mmio_base;
        
        // Set drawing color (example register offset)
        cmd_regs[0x1000/4] = color;
        
        // Set rectangle coordinates (example register offsets)
        cmd_regs[0x1004/4] = (uint32_t)x;
        cmd_regs[0x1008/4] = (uint32_t)y;
        cmd_regs[0x100C/4] = (uint32_t)w;
        cmd_regs[0x1010/4] = (uint32_t)h;
        
        // Execute fill command (example register)
        cmd_regs[0x1014/4] = 0x1; // Start fill operation
        
        printf("AMD GPU: Hardware fill command sent via MMIO\n");
    }
    
    // Also do direct VRAM access as backup/verification
    if (vram_base) {
        printf("AMD GPU: Direct VRAM access for immediate drawing\n");
        volatile uint32_t *screen = (volatile uint32_t*)vram_base;
        uint32_t pitch = gpu_width; // Assuming 32bpp
        
        for (uint64_t j = 0; j < h; j++) {
            for (uint64_t i = 0; i < w; i++) {
                screen[(y + j) * pitch + (x + i)] = color;
            }
        }
    }
}

void amd_gpu_init(void) {
#if !ENABLE_AMD_GPU
    printf("AMD GPU: driver disabled (ENABLE_AMD_GPU=0), using boot framebuffer\n");
    return;
#endif
    printf("AMD GPU: Initializing\n");
    DEBUG_SERIAL("AMD GPU: Initializing\n");

    struct pci_device *amdgpu_dev = NULL;

    for(int i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = pci_devices[i];
        if(dev->class_code == 0x03 && dev->subclass_code == 0x00 && dev->vendor_id == 0x1002) {
            printf("AMD GPU: Found AMD VGA device: device_id=0x%x at bus=%d dev=%d func=%d\n",
                   dev->device_id, dev->bus, dev->device, dev->function);
            DEBUG_SERIAL("AMD GPU: Found AMD VGA device: device_id=0x%x at bus=%d dev=%d func=%d\n",
                         dev->device_id, dev->bus, dev->device, dev->function);

            amdgpu_dev = dev;
            amdgpu_device = dev; // Store globally
            break;  // Assuming single GPU for now
        }
    }

    if (!amdgpu_dev) {
        printf("AMD GPU: No AMD VGA device found\n");
        return;
    }

    // Enable memory space and bus mastering
    uint16_t cmd = pci_device_read(amdgpu_dev, 0x04, 2);
    cmd |= 0x06;  // Bit 1: Memory space, Bit 2: Bus master
    pci_device_write(amdgpu_dev, 0x04, 2, cmd);

    printf("AMD GPU: Basic PCI configuration enabled\n");

    // Read and map BARs
    uint32_t bar0 = 0, bar5 = 0;
    
    for (int bar = 0; bar < 6; bar++) {
        uint32_t offset = 0x10 + bar * 4;
        uint32_t original = pci_device_read(amdgpu_dev, offset, 4);
        pci_device_write(amdgpu_dev, offset, 4, 0xFFFFFFFF);
        uint32_t size_mask = pci_device_read(amdgpu_dev, offset, 4);
        pci_device_write(amdgpu_dev, offset, 4, original);

        if (size_mask == 0) continue;

        bool is_io = size_mask & 1;
        uint32_t size = is_io ? ~(size_mask & ~3) + 1 : ~(size_mask & ~0xF) + 1;
        printf("AMD GPU: BAR%d: addr=0x%x, size=0x%x, type=%s\n",
               bar, original & ~ (is_io ? 3 : 0xF), size, is_io ? "IO" : "Memory");
        
        // Store important BARs
        if (bar == 0) bar0 = original & ~0xF; // VRAM
        if (bar == 5) bar5 = original & ~0xF; // MMIO
    }

    // Get framebuffer dimensions from system
    extern uint32_t fb_get_width(void);
    extern uint32_t fb_get_height(void);
    gpu_width = fb_get_width();
    gpu_height = fb_get_height();
    
    if (gpu_width == 0 || gpu_height == 0) {
        printf("AMD GPU: Warning - Could not get framebuffer dimensions\n");
        gpu_width = 1920; // Default fallback
        gpu_height = 1080;
    }
    
    printf("AMD GPU: Using framebuffer dimensions: %ux%u\n", gpu_width, gpu_height);

    // Map VRAM and MMIO
    if (bar0 != 0) {
        vram_base = (void*)(bar0 + vmm_higher_half_offset);
        printf("AMD GPU: VRAM mapped at %p (phys: 0x%x)\n", vram_base, bar0);
    }
    
    if (bar5 != 0) {
        mmio_base = (void*)(bar5 + vmm_higher_half_offset);
        printf("AMD GPU: MMIO mapped at %p (phys: 0x%x)\n", mmio_base, bar5);
    }

    printf("AMD GPU: BARs read and mapped\n");

    // After handling BARs
    uint32_t rom_base = pci_mm_read(amdgpu_dev, 0x30) & ~0xFFFFF; // Mask to get base, assuming 1MB alignment or similar
    pci_mm_write(amdgpu_dev, 0x30, rom_base | 1); // Enable ROM

    void* rom_mapped = (void*)(rom_base + vmm_higher_half_offset);
    if (rom_mapped) {
        char* magic = (char*)(rom_mapped + 0x04);
        if (magic[0] == 'A' && magic[1] == 'T' && magic[2] == 'O' && magic[3] == 'M') {
            printf("AtomBIOS ROM found at 0x%llx (virt)\n", (uint64_t)rom_base);
        } else {
            printf("No AtomBIOS magic found\n");
        }
    }

    // Disable ROM after use
    pci_mm_write(amdgpu_dev, 0x30, rom_base);

    // Access sample registers through existing mappings
    if (mmio_base) {
        printf("AMD GPU: MMIO mapped at %p (virt)\n", mmio_base);

        // Read a sample register, e.g., device ID from MMIO if applicable
        // For demo, read first dword
        uint32_t sample_reg = *(volatile uint32_t*)mmio_base;
        printf("AMD GPU: Sample MMIO read: 0x%x\n", sample_reg);
    }

    // Parse AtomBIOS header
    uint16_t bios_magic = *(uint16_t*)rom_mapped;
    if (bios_magic != 0xAA55) {
        printf("AMD GPU: Invalid BIOS magic: 0x%x\n", bios_magic);
    } else {
        uint16_t atom_rom_offset = *(uint16_t*)(rom_mapped + 0x48);
        void* atom_rom_table = rom_mapped + atom_rom_offset;

        uint16_t table_size = *(uint16_t*)atom_rom_table;
        uint8_t format_rev = *(uint8_t*)(atom_rom_table + 2);
        uint8_t content_rev = *(uint8_t*)(atom_rom_table + 3);

        uint32_t atom_magic = *(uint32_t*)(atom_rom_table + 4);
        if (atom_magic == *(uint32_t*)"ATOM") {
            printf("AMD GPU: AtomBIOS found! Table offset: 0x%x, size: 0x%x, format rev: %d, content rev: %d\n",
                   atom_rom_offset, table_size, format_rev, content_rev);

            // Optionally, read more fields
            uint16_t cmd_table_offset = *(uint16_t*)(atom_rom_table + 0x1E);
            printf("AMD GPU: Command table offset: 0x%x\n", cmd_table_offset);

            // Get ASIC_Init offset (index 0)
            void* cmd_table = rom_mapped + cmd_table_offset;
            uint16_t asic_init_offset = *(uint16_t*)(cmd_table + 4);  // After common header
            printf("AMD GPU: ASIC_Init command offset: 0x%x\n", asic_init_offset);

            // Execute ASIC_Init
            int ret = atom_execute(rom_mapped, asic_init_offset, NULL);
            if (ret == 0) {
                printf("AMD GPU: ASIC_Init executed successfully\n");
            } else {
                printf("AMD GPU: ASIC_Init failed: %d\n", ret);
            }
        } else {
            printf("AMD GPU: No AtomBIOS magic found: 0x%x\n", atom_magic);
        }
    }

    printf("AMD GPU: Initialization complete\n");
    gpu_initialized = true;
} 