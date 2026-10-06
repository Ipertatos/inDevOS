/**
 * @file virtio_gpu.h
 * @brief Virtio GPU driver for QEMU
 *
 * Implements the virtio-gpu protocol for hardware-accelerated 2D display
 * in QEMU. Supports resource creation, framebuffer attachment, scanout
 * configuration, and display updates.
 */

#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "types.h"
#include "virtio.h"

/* ============================================================================
 * Virtio GPU Command Types
 * ============================================================================ */

/* 2D commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109
#define VIRTIO_GPU_CMD_GET_EDID                0x010A

/* Cursor commands */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR           0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR             0x0301

/* Success/error responses */
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO         0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET              0x1103
#define VIRTIO_GPU_RESP_OK_EDID                0x1104
#define VIRTIO_GPU_RESP_ERR_UNSPEC             0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY      0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER  0x1205

/* Pixel formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM       1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM       3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM       4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM       67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM       68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM       121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM       134

/* Feature bits */
#define VIRTIO_GPU_F_VIRGL          0   /* 3D virgl support */
#define VIRTIO_GPU_F_EDID           1   /* EDID support */
#define VIRTIO_GPU_F_RESOURCE_UUID  2   /* Resource UUID support */

/* Maximum number of scanouts (displays) */
#define VIRTIO_GPU_MAX_SCANOUTS     16

/* ============================================================================
 * Virtio GPU Protocol Structures
 * ============================================================================ */

/* Common header for all GPU commands and responses */
typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

/* Rectangle */
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_rect_t;

/* Display info (per scanout) */
typedef struct {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one_t;

/* GET_DISPLAY_INFO response */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

/* RESOURCE_CREATE_2D */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

/* SET_SCANOUT */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

/* RESOURCE_FLUSH */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

/* TRANSFER_TO_HOST_2D */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

/* Memory entry for RESOURCE_ATTACH_BACKING */
typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

/* RESOURCE_ATTACH_BACKING */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

/* RESOURCE_UNREF */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

/* ============================================================================
 * Virtio GPU Device Configuration (device_cfg region)
 * ============================================================================ */

typedef struct {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
} __attribute__((packed)) virtio_gpu_config_t;

/* ============================================================================
 * Driver State
 * ============================================================================ */

typedef struct {
    virtio_device_t vdev;

    /* Display info */
    uint32_t width;
    uint32_t height;
    uint32_t num_scanouts;

    /* Framebuffer */
    uint32_t *framebuffer;
    uint64_t fb_phys;
    uint32_t fb_size;
    uint32_t pitch;           /* bytes per scanline */
    bool     owns_framebuffer; /* false when aliasing Limine FB */
    uint32_t resource_id;

    /* State */
    bool initialized;
    bool display_attached;
} virtio_gpu_t;

/* ============================================================================
 * API
 * ============================================================================ */

/**
 * @brief Initialize the virtio-GPU driver
 *
 * Scans PCI for a virtio-GPU device and initializes it.
 * Sets up the controlq and cursorq virtqueues, queries display
 * info, creates a 2D resource, and attaches framebuffer backing.
 *
 * @return 0 on success, -1 if no device found or init failed
 */
int virtio_gpu_init(void);

/**
 * @brief Check if virtio-GPU is available and initialized
 */
bool virtio_gpu_is_available(void);

/**
 * @brief Get the framebuffer address for direct pixel writes
 *
 * Returns a pointer to the BGRA8888 framebuffer. Write pixels
 * directly, then call virtio_gpu_flush() to update the display.
 *
 * @return Pointer to framebuffer, or NULL if not initialized
 */
uint32_t* virtio_gpu_get_framebuffer(void);

/**
 * @brief Get display dimensions
 */
uint32_t virtio_gpu_get_width(void);
uint32_t virtio_gpu_get_height(void);
uint32_t virtio_gpu_get_pitch(void);
/** True when virtio aliases the Limine/VGA FB (no SET_SCANOUT / flush needed). */
bool virtio_gpu_using_live_vga(void);

/**
 * @brief Flush a region of the framebuffer to the display
 *
 * Transfers the specified rectangle from host memory to the GPU
 * resource, then flushes (displays) it on the scanout.
 *
 * @param x, y       Top-left corner of the region
 * @param width, height  Size of the region
 */
void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

/**
 * @brief Flush the entire framebuffer to the display
 */
void virtio_gpu_flush_all(void);

/**
 * @brief Fill a rectangle with a solid color
 *
 * @param x, y       Top-left corner
 * @param w, h       Width and height
 * @param color      ARGB color value
 */
void virtio_gpu_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t color);

/**
 * @brief Clear the entire screen with a color
 */
void virtio_gpu_clear(uint32_t color);

/**
 * @brief Resize the virtio scanout buffer (guest drawing resolution).
 *
 * Used when the logical framebuffer size changes to match Limine/SDL.
 * Returns 0 on success, -1 on failure or if virtio-gpu is not initialized.
 */
int virtio_gpu_set_viewport_size(uint32_t width, uint32_t height);

#endif /* VIRTIO_GPU_H */
