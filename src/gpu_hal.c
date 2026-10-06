/**
 * @file gpu_hal.c
 * @brief GPU Hardware Abstraction Layer — registry, backends, dispatch
 *
 * All display backends are thin wrappers around their existing driver
 * APIs.  The HAL probes each one during gpu_hal_init() and activates
 * the best available backend.
 *
 * Probe priority (highest first):
 *   1. virtio-gpu  (preferred for QEMU)
 *   2. AMD GPU     (bare-metal discrete)
 *   3. VMware SVGA (VMware guests)
 *   4. Limine FB   (always-present fallback)
 */

#include "gpu_hal.h"
#include "mem.h"
#include "string.h"

/* Driver headers — each exposes its own init/query API. */
#include "virtio_gpu.h"
#include "vmware_svga.h"
#include "amd_gpu.h"

/* Limine framebuffer (defined in utils.c) */
#include "limine.h"
extern struct limine_framebuffer *fb;

/* Logging */
extern int  printf(const char *fmt, ...);
extern void serial_printf(const char *fmt, ...);

/* ============================================================================
 * Registry
 * ============================================================================ */

static gpu_device_t  devices[GPU_MAX_DEVICES];
static uint32_t      device_count = 0;
static gpu_device_t *active_device = (void *)0;

/* ============================================================================
 * Backend: Limine linear framebuffer
 * ============================================================================ */

static int limine_probe(void)
{
    return fb != (void *)0;
}

static int limine_init(gpu_device_t *dev)
{
    if (!fb) return -1;

    gpu_surface_t *s = &dev->primary_surface;
    s->handle  = 0;
    s->pixels  = (uint32_t *)fb->address;
    s->width   = (uint32_t)fb->width;
    s->height  = (uint32_t)fb->height;
    s->pitch   = (uint32_t)fb->pitch;
    s->bpp     = (uint32_t)fb->bpp;
    s->format  = 0; /* XRGB8888 assumed */
    s->size    = (uint64_t)fb->pitch * fb->height;
    s->driver_private = (void *)0;

    dev->caps = GPU_CAP_SCANOUT;
    return 0;
}

static void limine_shutdown(gpu_device_t *dev) { (void)dev; }

static gpu_surface_t *limine_get_primary(gpu_device_t *dev)
{
    return &dev->primary_surface;
}

static void limine_present_rect(gpu_device_t *dev,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h)
{
    (void)dev; (void)x; (void)y; (void)w; (void)h;
    /* Linear framebuffer — writes are immediately visible. */
}

static void limine_present_all(gpu_device_t *dev) { (void)dev; }

static void limine_fill_rect(gpu_device_t *dev,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h, uint32_t color)
{
    gpu_surface_t *s = &dev->primary_surface;
    if (!s->pixels) return;

    uint32_t bpp = s->bpp;
    uint32_t pitch = s->pitch;

    for (uint32_t row = y; row < y + h && row < s->height; row++) {
        for (uint32_t col = x; col < x + w && col < s->width; col++) {
            if (bpp == 32) {
                uint32_t *px = (uint32_t *)((uint8_t *)s->pixels + row * pitch + col * 4);
                *px = color;
            } else if (bpp == 24) {
                uint8_t *base = (uint8_t *)s->pixels + row * pitch + col * 3;
                base[0] = (uint8_t)(color & 0xFF);
                base[1] = (uint8_t)((color >> 8) & 0xFF);
                base[2] = (uint8_t)((color >> 16) & 0xFF);
            }
        }
    }
}

static void limine_clear(gpu_device_t *dev, uint32_t color)
{
    gpu_surface_t *s = &dev->primary_surface;
    if (!s->pixels) return;

    if (color == 0 && s->bpp == 32) {
        memset(s->pixels, 0, (uint64_t)s->pitch * s->height);
    } else {
        limine_fill_rect(dev, 0, 0, s->width, s->height, color);
    }
}

static uint32_t limine_get_caps(gpu_device_t *dev) { return dev->caps; }
static uint32_t limine_get_w(gpu_device_t *dev)    { return dev->primary_surface.width; }
static uint32_t limine_get_h(gpu_device_t *dev)    { return dev->primary_surface.height; }

static const gpu_device_ops_t limine_ops = {
    .name               = "limine-fb",
    .probe              = limine_probe,
    .init               = limine_init,
    .shutdown           = limine_shutdown,
    .get_primary_surface= limine_get_primary,
    .present_rect       = limine_present_rect,
    .present_all        = limine_present_all,
    .fill_rect          = limine_fill_rect,
    .clear              = limine_clear,
    .get_capabilities   = limine_get_caps,
    .get_width          = limine_get_w,
    .get_height         = limine_get_h,
};

/* ============================================================================
 * Backend: virtio-gpu 2D
 * ============================================================================ */

static int virtio_probe(void)
{
    return virtio_gpu_is_available() ? 1 : 0;
}

static int virtio_init(gpu_device_t *dev)
{
    uint32_t *pixels = virtio_gpu_get_framebuffer();
    if (!pixels) return -1;

    uint32_t w = virtio_gpu_get_width();
    uint32_t h = virtio_gpu_get_height();

    gpu_surface_t *s = &dev->primary_surface;
    s->handle  = 1;
    s->pixels  = pixels;
    s->width   = w;
    s->height  = h;
    s->pitch   = virtio_gpu_get_pitch();
    s->bpp     = 32;
    s->format  = 0;
    s->size    = (uint64_t)s->pitch * h;
    s->driver_private = (void *)0;

    dev->caps = GPU_CAP_SCANOUT | GPU_CAP_2D;
    return 0;
}

static void virtio_shutdown(gpu_device_t *dev) { (void)dev; }

static gpu_surface_t *virtio_get_primary(gpu_device_t *dev)
{
    return &dev->primary_surface;
}

static void virtio_present_rect(gpu_device_t *dev,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h)
{
    (void)dev;
    virtio_gpu_flush(x, y, w, h);
}

static void virtio_present_all(gpu_device_t *dev)
{
    (void)dev;
    virtio_gpu_flush_all();
}

static void virtio_fill_rect(gpu_device_t *dev,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h, uint32_t color)
{
    (void)dev;
    virtio_gpu_fill_rect(x, y, w, h, color);
}

static void virtio_clear(gpu_device_t *dev, uint32_t color)
{
    (void)dev;
    virtio_gpu_clear(color);
}

static uint32_t virtio_get_caps(gpu_device_t *dev) { return dev->caps; }
static uint32_t virtio_get_w(gpu_device_t *dev)    { (void)dev; return virtio_gpu_get_width(); }
static uint32_t virtio_get_h(gpu_device_t *dev)    { (void)dev; return virtio_gpu_get_height(); }

static const gpu_device_ops_t virtio_ops = {
    .name               = "virtio-gpu",
    .probe              = virtio_probe,
    .init               = virtio_init,
    .shutdown           = virtio_shutdown,
    .get_primary_surface= virtio_get_primary,
    .present_rect       = virtio_present_rect,
    .present_all        = virtio_present_all,
    .fill_rect          = virtio_fill_rect,
    .clear              = virtio_clear,
    .get_capabilities   = virtio_get_caps,
    .get_width          = virtio_get_w,
    .get_height         = virtio_get_h,
};

/* ============================================================================
 * Backend: AMD GPU
 * ============================================================================ */

static int amd_probe(void)
{
    return amd_gpu_is_available() ? 1 : 0;
}

static int amd_init(gpu_device_t *dev)
{
    uint32_t w = 0, h = 0;
    amd_gpu_get_info(&w, &h);
    if (w == 0 || h == 0) return -1;

    gpu_surface_t *s = &dev->primary_surface;
    s->handle  = 2;
    s->pixels  = (void *)0;   /* AMD may write through VRAM directly */
    s->width   = w;
    s->height  = h;
    s->pitch   = w * 4;
    s->bpp     = 32;
    s->format  = 0;
    s->size    = (uint64_t)w * h * 4;
    s->driver_private = (void *)0;

    /* Software VRAM loops, not HW 2D. */
    dev->caps = GPU_CAP_SCANOUT;
    return 0;
}

static void amd_shutdown(gpu_device_t *dev) { (void)dev; }

static gpu_surface_t *amd_get_primary(gpu_device_t *dev)
{
    return &dev->primary_surface;
}

static void amd_present_rect(gpu_device_t *dev,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h)
{
    (void)dev; (void)x; (void)y; (void)w; (void)h;
    /* AMD direct VRAM writes are immediately visible on scanout. */
}

static void amd_present_all(gpu_device_t *dev) { (void)dev; }

static void amd_fill_rect(gpu_device_t *dev,
                          uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, uint32_t color)
{
    (void)dev;
    amd_gpu_fillrect(color, (uint64_t)x, (uint64_t)y,
                     (uint64_t)w, (uint64_t)h);
}

static void amd_clear(gpu_device_t *dev, uint32_t color)
{
    (void)dev;
    amd_gpu_clear_screen(color);
}

static uint32_t amd_get_caps(gpu_device_t *dev) { return dev->caps; }

static uint32_t amd_get_w(gpu_device_t *dev)
{
    uint32_t w = 0, h = 0;
    (void)dev;
    amd_gpu_get_info(&w, &h);
    return w;
}

static uint32_t amd_get_h(gpu_device_t *dev)
{
    uint32_t w = 0, h = 0;
    (void)dev;
    amd_gpu_get_info(&w, &h);
    return h;
}

static const gpu_device_ops_t amd_ops = {
    .name               = "amd-gpu",
    .probe              = amd_probe,
    .init               = amd_init,
    .shutdown           = amd_shutdown,
    .get_primary_surface= amd_get_primary,
    .present_rect       = amd_present_rect,
    .present_all        = amd_present_all,
    .fill_rect          = amd_fill_rect,
    .clear              = amd_clear,
    .get_capabilities   = amd_get_caps,
    .get_width          = amd_get_w,
    .get_height         = amd_get_h,
};

/* ============================================================================
 * Backend: VMware SVGA-II
 * ============================================================================ */

static int svga_probe(void)
{
    return vmware_svga_is_available() ? 1 : 0;
}

static int svga_init(gpu_device_t *dev)
{
    uint32_t w = 0, h = 0;
    vmware_svga_get_info(&w, &h);
    if (w == 0 || h == 0) return -1;

    gpu_surface_t *s = &dev->primary_surface;
    s->handle  = 3;
    s->pixels  = (void *)0;
    s->width   = w;
    s->height  = h;
    s->pitch   = w * 4;
    s->bpp     = 32;
    s->format  = 0;
    s->size    = (uint64_t)w * h * 4;
    s->driver_private = (void *)0;

    /* SVGA accessors are stubs; do not advertise 2D accel. */
    dev->caps = GPU_CAP_SCANOUT;
    return 0;
}

static void svga_shutdown(gpu_device_t *dev) { (void)dev; }

static gpu_surface_t *svga_get_primary(gpu_device_t *dev)
{
    return &dev->primary_surface;
}

static void svga_present_rect(gpu_device_t *dev,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    (void)dev; (void)x; (void)y; (void)w; (void)h;
}

static void svga_present_all(gpu_device_t *dev) { (void)dev; }

static void svga_fill_rect(gpu_device_t *dev,
                           uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, uint32_t color)
{
    (void)dev;
    vmware_svga_fillrect(color, x, y, w, h);
}

static void svga_clear(gpu_device_t *dev, uint32_t color)
{
    (void)dev;
    vmware_svga_clear_screen(color);
}

static uint32_t svga_get_caps(gpu_device_t *dev) { return dev->caps; }

static uint32_t svga_get_w(gpu_device_t *dev)
{
    uint32_t w = 0, h = 0;
    (void)dev;
    vmware_svga_get_info(&w, &h);
    return w;
}

static uint32_t svga_get_h(gpu_device_t *dev)
{
    uint32_t w = 0, h = 0;
    (void)dev;
    vmware_svga_get_info(&w, &h);
    return h;
}

static const gpu_device_ops_t svga_ops = {
    .name               = "vmware-svga",
    .probe              = svga_probe,
    .init               = svga_init,
    .shutdown           = svga_shutdown,
    .get_primary_surface= svga_get_primary,
    .present_rect       = svga_present_rect,
    .present_all        = svga_present_all,
    .fill_rect          = svga_fill_rect,
    .clear              = svga_clear,
    .get_capabilities   = svga_get_caps,
    .get_width          = svga_get_w,
    .get_height         = svga_get_h,
};

/* ============================================================================
 * Ordered probe table  (highest priority first)
 * ============================================================================ */

static const gpu_device_ops_t *backend_table[] = {
    &virtio_ops,
    &amd_ops,
    &svga_ops,
    &limine_ops,
};

#define BACKEND_COUNT (sizeof(backend_table) / sizeof(backend_table[0]))

/* ============================================================================
 * HAL management
 * ============================================================================ */

int gpu_hal_register_backend(const gpu_device_ops_t *ops)
{
    if (device_count >= GPU_MAX_DEVICES) return -1;

    gpu_device_t *dev = &devices[device_count];
    memset(dev, 0, sizeof(*dev));
    dev->ops = ops;
    dev->id  = device_count;
    device_count++;
    return 0;
}

void gpu_hal_init(void)
{
    device_count  = 0;
    active_device = (void *)0;

    for (uint32_t i = 0; i < BACKEND_COUNT; i++) {
        const gpu_device_ops_t *ops = backend_table[i];

        if (!ops->probe || !ops->probe()) {
            serial_printf("[gpu_hal] %s: not present\n", ops->name);
            continue;
        }

        gpu_device_t *dev = &devices[device_count];
        memset(dev, 0, sizeof(*dev));
        dev->ops = ops;
        dev->id  = device_count;

        if (ops->init && ops->init(dev) < 0) {
            serial_printf("[gpu_hal] %s: probe OK, init FAILED\n", ops->name);
            continue;
        }

        if (ops->get_capabilities)
            dev->caps = ops->get_capabilities(dev);

        serial_printf("[gpu_hal] %s: registered caps=0x%x %ux%u\n",
                      ops->name, dev->caps,
                      ops->get_width  ? ops->get_width(dev)  : 0,
                      ops->get_height ? ops->get_height(dev) : 0);

        if (!active_device) {
            dev->active   = true;
            active_device = dev;
        }

        device_count++;
    }

    gpu_hal_log_capabilities();
}

gpu_device_t *gpu_hal_get_active(void)
{
    return active_device;
}

gpu_device_t *gpu_hal_get_device(uint32_t index)
{
    if (index >= device_count) return (void *)0;
    return &devices[index];
}

uint32_t gpu_hal_device_count(void)
{
    return device_count;
}

/* ============================================================================
 * Convenience dispatchers
 * ============================================================================ */

gpu_surface_t *gpu_hal_get_primary_surface(void)
{
    if (!active_device || !active_device->ops->get_primary_surface)
        return (void *)0;
    return active_device->ops->get_primary_surface(active_device);
}

void gpu_hal_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (active_device && active_device->ops->present_rect)
        active_device->ops->present_rect(active_device, x, y, w, h);
}

void gpu_hal_present_all(void)
{
    if (active_device && active_device->ops->present_all)
        active_device->ops->present_all(active_device);
}

void gpu_hal_fill_rect(uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h, uint32_t color)
{
    if (active_device && active_device->ops->fill_rect)
        active_device->ops->fill_rect(active_device, x, y, w, h, color);
}

void gpu_hal_clear(uint32_t color)
{
    if (active_device && active_device->ops->clear)
        active_device->ops->clear(active_device, color);
}

uint32_t gpu_hal_get_width(void)
{
    if (active_device && active_device->ops->get_width)
        return active_device->ops->get_width(active_device);
    return 0;
}

uint32_t gpu_hal_get_height(void)
{
    if (active_device && active_device->ops->get_height)
        return active_device->ops->get_height(active_device);
    return 0;
}

uint32_t gpu_hal_get_capabilities(void)
{
    if (active_device)
        return active_device->caps;
    return 0;
}

const char *gpu_hal_get_backend_name(void)
{
    if (active_device && active_device->ops)
        return active_device->ops->name;
    return "none";
}

/* ============================================================================
 * Capability logging (Step 4 of the roadmap)
 * ============================================================================ */

void gpu_hal_sync_active_virtio_surface(void)
{
    if (!active_device || !active_device->ops ||
        kstrcmp(active_device->ops->name, "virtio-gpu") != 0)
        return;

    uint32_t *pixels = virtio_gpu_get_framebuffer();
    if (!pixels)
        return;

    uint32_t w = virtio_gpu_get_width();
    uint32_t h = virtio_gpu_get_height();
    gpu_surface_t *s = &active_device->primary_surface;
    s->pixels  = pixels;
    s->width   = w;
    s->height  = h;
    s->pitch   = virtio_gpu_get_pitch();
    s->size    = (uint64_t)s->pitch * h;
}

void gpu_hal_log_capabilities(void)
{
    if (!active_device) {
        printf("GPU: no display backend available\n");
        return;
    }

    uint32_t caps = active_device->caps;
    uint32_t w = active_device->ops->get_width
                 ? active_device->ops->get_width(active_device) : 0;
    uint32_t h = active_device->ops->get_height
                 ? active_device->ops->get_height(active_device) : 0;

    /* Build caps string in a fixed buffer to avoid interleaved output */
    char capbuf[128];
    int pos = 0;
    struct { uint32_t bit; const char *tag; } cap_names[] = {
        { GPU_CAP_SCANOUT, "SCANOUT" },
        { GPU_CAP_2D,      "2D" },
        { GPU_CAP_3D,      "3D" },
        { GPU_CAP_VIRGL,   "VIRGL" },
        { GPU_CAP_VULKAN,  "VULKAN" },
        { GPU_CAP_COMPUTE, "COMPUTE" },
        { GPU_CAP_USERPTR, "USERPTR" },
        { GPU_CAP_DMA,     "DMA" },
    };

    for (uint32_t i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
        if (caps & cap_names[i].bit) {
            if (pos > 0 && pos < 126)
                capbuf[pos++] = ',';
            for (const char *t = cap_names[i].tag; *t && pos < 127; t++)
                capbuf[pos++] = *t;
        }
    }
    if (pos == 0) {
        capbuf[0] = '-'; pos = 1;
    }
    capbuf[pos] = '\0';

    printf("GPU: backend=%s %ux%u caps=%s\n",
           active_device->ops->name, w, h, capbuf);

    if (device_count > 1)
        printf("GPU: %u backends registered\n", device_count);
}
