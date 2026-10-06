#pragma once

#include "types.h"
#include "pci.h"

// VMware SVGA-II PCI IDs
#define VMWARE_VENDOR_ID    0x15AD
#define VMWARE_SVGA_DEVICE_ID 0x0405

// VMware SVGA-II PCI Class/Subclass
#define VMWARE_SVGA_CLASS    0x03  // Display Controller
#define VMWARE_SVGA_SUBCLASS 0x00  // VGA Compatible

// SVGA registers
#define SVGA_REG_ID                     0x00
#define SVGA_REG_ENABLE                 0x01
#define SVGA_REG_WIDTH                  0x02
#define SVGA_REG_HEIGHT                 0x03
#define SVGA_REG_MAX_WIDTH              0x04
#define SVGA_REG_MAX_HEIGHT             0x05
#define SVGA_REG_DEPTH                  0x06
#define SVGA_REG_BITS_PER_PIXEL         0x07
#define SVGA_REG_PSEUDOCOLOR            0x08
#define SVGA_REG_RED_MASK               0x09
#define SVGA_REG_GREEN_MASK             0x0A
#define SVGA_REG_BLUE_MASK              0x0B
#define SVGA_REG_BYTES_PER_LINE         0x0C
#define SVGA_REG_FB_START               0x0D
#define SVGA_REG_FB_OFFSET              0x0E
#define SVGA_REG_VRAM_SIZE              0x0F
#define SVGA_REG_FB_SIZE                0x10

// FIFO registers
#define SVGA_REG_CAPABILITIES           0x11
#define SVGA_REG_MEM_START              0x12
#define SVGA_REG_MEM_SIZE               0x13
#define SVGA_REG_CONFIG_DONE            0x14
#define SVGA_REG_SYNC                   0x15
#define SVGA_REG_BUSY                   0x16
#define SVGA_REG_GUEST_ID               0x17
#define SVGA_REG_CURSOR_ID              0x18
#define SVGA_REG_CURSOR_X               0x19
#define SVGA_REG_CURSOR_Y               0x1A
#define SVGA_REG_CURSOR_ON              0x1B
#define SVGA_REG_HOST_BITS_PER_PIXEL    0x1C
#define SVGA_REG_SCRATCH_SIZE           0x1D
#define SVGA_REG_MEM_REGS               0x1E
#define SVGA_REG_NUM_DISPLAYS           0x1F
#define SVGA_REG_PITCHLOCK              0x20

// SVGA ID constants
#define SVGA_ID_2                       0x90000002
#define SVGA_ID_1                       0x90000001
#define SVGA_ID_0                       0x90000000

// SVGA capabilities
#define SVGA_CAP_NONE                   0x00000000
#define SVGA_CAP_RECT_COPY              0x00000002
#define SVGA_CAP_RECT_FILL              0x00000004
#define SVGA_CAP_RECT_PAT_FILL          0x00000008
#define SVGA_CAP_LEGACY_OFFSCREEN       0x00000010
#define SVGA_CAP_RASTER_OP              0x00000020
#define SVGA_CAP_CURSOR                 0x00000040
#define SVGA_CAP_CURSOR_BYPASS          0x00000080
#define SVGA_CAP_CURSOR_BYPASS_2        0x00000100
#define SVGA_CAP_8BIT_EMULATION         0x00000200
#define SVGA_CAP_ALPHA_CURSOR           0x00000400
#define SVGA_CAP_3D                     0x00004000
#define SVGA_CAP_EXTENDED_FIFO          0x00008000
#define SVGA_CAP_MULTIMON               0x00010000
#define SVGA_CAP_PITCHLOCK              0x00020000
#define SVGA_CAP_IRQMASK                0x00040000
#define SVGA_CAP_DISPLAY_TOPOLOGY       0x00080000
#define SVGA_CAP_GMR                    0x00100000
#define SVGA_CAP_TRACES                 0x00200000

// FIFO offsets
#define SVGA_FIFO_MIN                   0
#define SVGA_FIFO_MAX                   1
#define SVGA_FIFO_NEXT_CMD              2
#define SVGA_FIFO_STOP                  3
#define SVGA_FIFO_CAPABILITIES          4
#define SVGA_FIFO_FLAGS                 5
#define SVGA_FIFO_FENCE                 6
#define SVGA_FIFO_3D_HWVERSION          7
#define SVGA_FIFO_PITCHLOCK             8
#define SVGA_FIFO_CURSOR_ON             9
#define SVGA_FIFO_CURSOR_X              10
#define SVGA_FIFO_CURSOR_Y              11
#define SVGA_FIFO_CURSOR_COUNT          12
#define SVGA_FIFO_CURSOR_LAST_UPDATED   13
#define SVGA_FIFO_RESERVED              14
#define SVGA_FIFO_CURSOR_SCREEN_ID      15
#define SVGA_FIFO_DEAD                  16
#define SVGA_FIFO_3D_HWVERSION_REVISED  17

// FIFO commands
#define SVGA_CMD_INVALID_CMD            0
#define SVGA_CMD_UPDATE                 1
#define SVGA_CMD_RECT_COPY              3
#define SVGA_CMD_RECT_ROP_COPY          14
#define SVGA_CMD_DEFINE_BITMAP          5
#define SVGA_CMD_DEFINE_BITMAP_SCANLINE 6
#define SVGA_CMD_DEFINE_PIXMAP          7
#define SVGA_CMD_DEFINE_PIXMAP_SCANLINE 8
#define SVGA_CMD_RECT_BITMAP_FILL       9
#define SVGA_CMD_RECT_PIXMAP_FILL       10
#define SVGA_CMD_RECT_BITMAP_COPY       11
#define SVGA_CMD_RECT_PIXMAP_COPY       12
#define SVGA_CMD_FREE_OBJECT            13
#define SVGA_CMD_RECT_ROP_BITMAP_FILL   15
#define SVGA_CMD_RECT_ROP_PIXMAP_FILL   16
#define SVGA_CMD_RECT_ROP_BITMAP_COPY   17
#define SVGA_CMD_RECT_ROP_PIXMAP_COPY   18
#define SVGA_CMD_DEFINE_CURSOR          19
#define SVGA_CMD_DISPLAY_CURSOR         20
#define SVGA_CMD_MOVE_CURSOR            21
#define SVGA_CMD_DEFINE_ALPHA_CURSOR    22
#define SVGA_CMD_UPDATE_VERBOSE         25
#define SVGA_CMD_FRONT_ROP_FILL         29
#define SVGA_CMD_RECT_ROP_FILL          29  // Alias for FRONT_ROP_FILL
#define SVGA_CMD_FENCE                  30
#define SVGA_CMD_ESCAPE                 31

// I/O port offsets (relative to BAR0 - 1)
#define SVGA_INDEX_PORT                 0x0
#define SVGA_VALUE_PORT                 0x1
#define SVGA_BIOS_PORT                  0x2
#define SVGA_IRQSTATUS_PORT             0x8

// Magic numbers
#define SVGA_MAGIC                      0x900000UL
#define SVGA_MAKE_ID(ver)               (SVGA_MAGIC << 8 | (ver))

// Structure definitions
typedef struct {
    struct pci_device *pci_dev;
    uint16_t io_base;                   // I/O port base address
    uint8_t *framebuffer;               // Framebuffer virtual address
    uint32_t *fifo;                     // FIFO virtual address
    uint32_t framebuffer_size;
    uint32_t fifo_size;
    uint32_t vram_size;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t capabilities;
    bool initialized;
    bool enabled;
} vmware_svga_device_t;

// Function prototypes
void vmware_svga_init(void);
bool vmware_svga_is_available(void);
void vmware_svga_get_info(uint32_t *width, uint32_t *height);
void vmware_svga_clear_screen(uint32_t color);
void vmware_svga_fillrect(uint32_t color, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void vmware_svga_rect_copy(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);
int vmware_svga_set_mode(uint32_t width, uint32_t height, uint32_t bpp);

// Low-level register access
void vmware_svga_write_reg(uint32_t reg, uint32_t value);
uint32_t vmware_svga_read_reg(uint32_t reg);

// FIFO operations
void vmware_svga_fifo_init(void);
void vmware_svga_fifo_write(uint32_t value);
void vmware_svga_fifo_commit(void);
bool vmware_svga_fifo_full(void);

// Driver adapter functions (for driver management system)
int vmware_svga_driver_adapter_init(struct pci_device *dev);
int vmware_svga_driver_adapter_deinit(void);
int vmware_svga_driver_adapter_probe(struct pci_device *dev);
int vmware_svga_driver_adapter_enable(void);
int vmware_svga_driver_adapter_disable(void);
int vmware_svga_driver_adapter_reset(void);
void vmware_svga_driver_adapter_status(void);
