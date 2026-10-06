/**
 * @file gpu_hal.h
 * @brief GPU Hardware Abstraction Layer
 *
 * Provides a unified interface over all display backends (Limine FB,
 * virtio-gpu 2D, VMware SVGA, AMD GPU).  Every backend implements the
 * same ops vtable so that upper layers (SDL, term, compositor, and
 * eventually Mesa/DRM) never need to know which hardware is active.
 *
 * Designed to be GEM-like internally so mapping future Linux DRM ioctls
 * to HAL ops stays mechanical.
 *
 * Pinned Linux uAPI reference version: 6.6 LTS
 */

#ifndef GPU_HAL_H
#define GPU_HAL_H

#include "types.h"

/* ============================================================================
 * Capability flags
 * ============================================================================ */

#define GPU_CAP_SCANOUT     (1u << 0)  /* Can drive a physical scanout    */
#define GPU_CAP_2D          (1u << 1)  /* HW-accelerated 2D (fill, blit)  */
#define GPU_CAP_3D          (1u << 2)  /* 3D acceleration                 */
#define GPU_CAP_VIRGL       (1u << 3)  /* VirGL (virtio-gpu 3D passthru)  */
#define GPU_CAP_VULKAN      (1u << 4)  /* Vulkan-class support            */
#define GPU_CAP_COMPUTE     (1u << 5)  /* Compute shaders                 */
#define GPU_CAP_USERPTR     (1u << 6)  /* User-space-mappable buffers     */
#define GPU_CAP_DMA         (1u << 7)  /* DMA transfers                   */

/* ============================================================================
 * Surface / buffer object
 *
 * Wraps a pixel buffer that may live in system RAM (Limine, virtio
 * backing) or VRAM (AMD, future).  The handle is an opaque integer
 * that will map to a GEM handle once the DRM layer is active.
 * ============================================================================ */

typedef struct gpu_surface {
    uint32_t  handle;           /* Opaque handle (unique per device)  */
    uint32_t *pixels;           /* CPU-visible pixel pointer (or NULL)*/
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;            /* Bytes per scanline                 */
    uint32_t  bpp;              /* Bits per pixel                     */
    uint32_t  format;           /* 0 = XRGB8888 (default)             */
    uint64_t  size;             /* Total buffer bytes                 */
    void     *driver_private;   /* Backend-specific cookie            */
} gpu_surface_t;

/* ============================================================================
 * Device operations (vtable)
 * ============================================================================ */

struct gpu_device;

typedef struct gpu_device_ops {
    const char *name;

    /* Returns 1 if the backend's hardware is present, 0 otherwise.
       Called during gpu_hal_init(); the underlying driver must already
       have been initialized (virtio_gpu_init, amd_gpu_init, …). */
    int  (*probe)(void);

    /* One-time setup after probe succeeds. */
    int  (*init)(struct gpu_device *dev);

    /* Tear-down (unused today, placeholder for hot-unplug / kexec). */
    void (*shutdown)(struct gpu_device *dev);

    /* Return the device's primary scanout surface. */
    gpu_surface_t *(*get_primary_surface)(struct gpu_device *dev);

    /* Flush a rectangular region to the display.  For a linear FB
       (Limine) this is a no-op; for virtio-gpu it issues
       TRANSFER_TO_HOST + RESOURCE_FLUSH. */
    void (*present_rect)(struct gpu_device *dev,
                         uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h);
    void (*present_all)(struct gpu_device *dev);

    /* Optional 2D drawing ops (NULL = not supported). */
    void (*fill_rect)(struct gpu_device *dev,
                      uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t color);
    void (*clear)(struct gpu_device *dev, uint32_t color);

    /* Query helpers. */
    uint32_t (*get_capabilities)(struct gpu_device *dev);
    uint32_t (*get_width)(struct gpu_device *dev);
    uint32_t (*get_height)(struct gpu_device *dev);
} gpu_device_ops_t;

/* ============================================================================
 * GPU device instance
 * ============================================================================ */

#define GPU_MAX_DEVICES 8

typedef struct gpu_device {
    const gpu_device_ops_t *ops;
    uint32_t      caps;
    uint32_t      id;             /* Index in the registry            */
    bool          active;         /* Driving the primary display?     */
    void         *backend_data;   /* Driver-private state pointer     */
    gpu_surface_t primary_surface;
} gpu_device_t;

/* ============================================================================
 * HAL management API
 * ============================================================================ */

void           gpu_hal_init(void);

int            gpu_hal_register_backend(const gpu_device_ops_t *ops);
gpu_device_t  *gpu_hal_get_active(void);
gpu_device_t  *gpu_hal_get_device(uint32_t index);
uint32_t       gpu_hal_device_count(void);

/* Convenience wrappers that dispatch to the active device. */
gpu_surface_t *gpu_hal_get_primary_surface(void);
void           gpu_hal_present_rect(uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h);
void           gpu_hal_present_all(void);
void           gpu_hal_fill_rect(uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h, uint32_t color);
void           gpu_hal_clear(uint32_t color);
uint32_t       gpu_hal_get_width(void);
uint32_t       gpu_hal_get_height(void);
uint32_t       gpu_hal_get_capabilities(void);
const char    *gpu_hal_get_backend_name(void);

void           gpu_hal_log_capabilities(void);

/** Refresh primary_surface after virtio_gpu_set_viewport_size(). */
void           gpu_hal_sync_active_virtio_surface(void);

#endif /* GPU_HAL_H */
