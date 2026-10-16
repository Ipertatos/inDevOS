/**
 * @file virtio_gpu.c
 * @brief Virtio GPU driver implementation
 *
 * Implements the virtio-gpu 2D protocol for QEMU. Handles display
 * initialization, framebuffer management, and display updates.
 *
 * Virtqueues:
 *   Queue 0: controlq - GPU commands and responses
 *   Queue 1: cursorq  - Cursor updates
 */

#include "virtio_gpu.h"
#include "gpu_hal.h"
#include "limine.h"
#include "mem.h"
#include "serial.h"
#include "utils.h"
#include "pci.h"
#include "string.h"

extern uint64_t vmm_higher_half_offset;
extern struct limine_framebuffer *fb;

/* Global driver instance */
static virtio_gpu_t gpu;

/* Reused command/response buffers for per-frame transfer+flush (avoid
 * 4× kalloc/kfree on every SDL_RenderPresent / desktop fast-present). */
static virtio_gpu_transfer_to_host_2d_t virtio_xfer_cmd;
static virtio_gpu_resource_flush_t    virtio_flush_cmd;
static virtio_gpu_ctrl_hdr_t          virtio_xfer_resp;
static virtio_gpu_ctrl_hdr_t          virtio_flush_resp;

/* Next resource ID */
static uint32_t next_resource_id = 1;

/* Default display resolution */
#define DEFAULT_WIDTH   1024
#define DEFAULT_HEIGHT  768

/* ============================================================================
 * Command Submission
 * ============================================================================ */

/**
 * Submit a GPU command and wait for the response.
 * Sends the command on controlq (queue 0) and busy-waits for completion.
 *
 * @param cmd       Command buffer (device-readable)
 * @param cmd_len   Command length
 * @param resp      Response buffer (device-writable)
 * @param resp_len  Response buffer length
 * @return 0 on success, -1 on error
 */
static int gpu_submit_cmd(void *cmd, uint32_t cmd_len,
                           void *resp, uint32_t resp_len) {
    virtqueue_t *vq = &gpu.vdev.queues[0];

    /* Physical addresses for DMA */
    uint64_t cmd_phys  = (uint64_t)cmd  - vmm_higher_half_offset;
    uint64_t resp_phys = (uint64_t)resp - vmm_higher_half_offset;

    uint64_t bufs[2] = { cmd_phys, resp_phys };
    uint32_t lens[2] = { cmd_len, resp_len };

    /* 1 outgoing (command), 1 incoming (response) */
    int head = virtio_add_buf(vq, bufs, lens, 1, 1);
    if (head < 0) {
        serial_puts("virtio_gpu: failed to add command buffer\n");
        return -1;
    }

    /* Notify the device */
    virtio_kick(vq);

    /* Wait for response (with timeout) */
    for (int timeout = 0; timeout < 1000000; timeout++) {
        if (virtio_has_used(vq)) {
            uint32_t len;
            int used_id = virtio_get_used(vq, &len);
            if (used_id >= 0) {
                virtio_free_desc(vq, (uint16_t)used_id);
                return 0;
            }
        }
        __asm__ volatile("pause");
    }

    serial_puts("virtio_gpu: command timed out\n");
    return -1;
}

/* ============================================================================
 * GPU Commands
 * ============================================================================ */

static int gpu_get_display_info(void) {
    /* Allocate command and response buffers */
    virtio_gpu_ctrl_hdr_t *cmd = kalloc(sizeof(virtio_gpu_ctrl_hdr_t));
    virtio_gpu_resp_display_info_t *resp = kalloc(sizeof(virtio_gpu_resp_display_info_t));
    if (!cmd || !resp) {
        if (cmd) kfree(cmd);
        if (resp) kfree(resp);
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    int ret = gpu_submit_cmd(cmd, sizeof(*cmd), resp, sizeof(*resp));
    if (ret < 0 || resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        serial_printf("virtio_gpu: GET_DISPLAY_INFO failed (type=0x%x)\n",
                      resp->hdr.type);
        kfree(cmd);
        kfree(resp);
        return -1;
    }

    /* Find the first enabled scanout */
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (resp->pmodes[i].enabled) {
            gpu.width = resp->pmodes[i].r.width;
            gpu.height = resp->pmodes[i].r.height;
            serial_printf("virtio_gpu: scanout %d: %dx%d\n",
                          i, gpu.width, gpu.height);
            break;
        }
    }

    /* If no scanout is enabled, use defaults */
    if (gpu.width == 0 || gpu.height == 0) {
        gpu.width = DEFAULT_WIDTH;
        gpu.height = DEFAULT_HEIGHT;
        serial_printf("virtio_gpu: no enabled scanout, using %dx%d\n",
                      gpu.width, gpu.height);
    }

    kfree(cmd);
    kfree(resp);
    return 0;
}

static int gpu_create_resource(uint32_t resource_id, uint32_t format,
                                uint32_t width, uint32_t height) {
    virtio_gpu_resource_create_2d_t *cmd = kalloc(sizeof(*cmd));
    virtio_gpu_ctrl_hdr_t *resp = kalloc(sizeof(*resp));
    if (!cmd || !resp) {
        if (cmd) kfree(cmd);
        if (resp) kfree(resp);
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->resource_id = resource_id;
    cmd->format = format;
    cmd->width = width;
    cmd->height = height;

    int ret = gpu_submit_cmd(cmd, sizeof(*cmd), resp, sizeof(*resp));
    if (ret < 0 || resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("virtio_gpu: RESOURCE_CREATE_2D failed (type=0x%x)\n",
                      resp->type);
        kfree(cmd);
        kfree(resp);
        return -1;
    }

    serial_printf("virtio_gpu: created resource %d (%dx%d)\n",
                  resource_id, width, height);
    kfree(cmd);
    kfree(resp);
    return 0;
}

static int gpu_attach_backing(uint32_t resource_id, uint64_t phys_addr,
                               uint32_t length) {
    /* Command + 1 memory entry */
    uint32_t total_size = sizeof(virtio_gpu_resource_attach_backing_t) +
                          sizeof(virtio_gpu_mem_entry_t);
    uint8_t *buf = kalloc(total_size);
    virtio_gpu_ctrl_hdr_t *resp = kalloc(sizeof(*resp));
    if (!buf || !resp) {
        if (buf) kfree(buf);
        if (resp) kfree(resp);
        return -1;
    }

    memset(buf, 0, total_size);
    memset(resp, 0, sizeof(*resp));

    virtio_gpu_resource_attach_backing_t *cmd =
        (virtio_gpu_resource_attach_backing_t *)buf;
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = resource_id;
    cmd->nr_entries = 1;

    virtio_gpu_mem_entry_t *entry =
        (virtio_gpu_mem_entry_t *)(buf + sizeof(*cmd));
    entry->addr = phys_addr;
    entry->length = length;

    int ret = gpu_submit_cmd(buf, total_size, resp, sizeof(*resp));
    if (ret < 0 || resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("virtio_gpu: ATTACH_BACKING failed (type=0x%x)\n",
                      resp->type);
        kfree(buf);
        kfree(resp);
        return -1;
    }

    serial_printf("virtio_gpu: attached backing store for resource %d\n",
                  resource_id);
    kfree(buf);
    kfree(resp);
    return 0;
}

static int gpu_resource_detach_backing(uint32_t resource_id)
{
    typedef struct {
        virtio_gpu_ctrl_hdr_t hdr;
        uint32_t resource_id;
        uint32_t padding;
    } detach_cmd_t;

    detach_cmd_t *cmd = kalloc(sizeof(*cmd));
    virtio_gpu_ctrl_hdr_t *resp = kalloc(sizeof(*resp));
    if (!cmd || !resp) {
        if (cmd) kfree(cmd);
        if (resp) kfree(resp);
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd->resource_id = resource_id;

    int ret = gpu_submit_cmd(cmd, sizeof(*cmd), resp, sizeof(*resp));
    if (ret < 0 || resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("virtio_gpu: DETACH_BACKING failed (type=0x%x)\n", resp->type);
        kfree(cmd);
        kfree(resp);
        return -1;
    }
    kfree(cmd);
    kfree(resp);
    return 0;
}

static int gpu_resource_unref_id(uint32_t resource_id)
{
    virtio_gpu_resource_unref_t *cmd = kalloc(sizeof(*cmd));
    virtio_gpu_ctrl_hdr_t *resp = kalloc(sizeof(*resp));
    if (!cmd || !resp) {
        if (cmd) kfree(cmd);
        if (resp) kfree(resp);
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd->resource_id = resource_id;

    int ret = gpu_submit_cmd(cmd, sizeof(*cmd), resp, sizeof(*resp));
    if (ret < 0 || resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("virtio_gpu: RESOURCE_UNREF failed (type=0x%x)\n", resp->type);
        kfree(cmd);
        kfree(resp);
        return -1;
    }
    kfree(cmd);
    kfree(resp);
    return 0;
}

static int gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                            uint32_t width, uint32_t height) {
    virtio_gpu_set_scanout_t *cmd = kalloc(sizeof(*cmd));
    virtio_gpu_ctrl_hdr_t *resp = kalloc(sizeof(*resp));
    if (!cmd || !resp) {
        if (cmd) kfree(cmd);
        if (resp) kfree(resp);
        return -1;
    }

    memset(cmd, 0, sizeof(*cmd));
    memset(resp, 0, sizeof(*resp));
    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->r.x = 0;
    cmd->r.y = 0;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->scanout_id = scanout_id;
    cmd->resource_id = resource_id;

    int ret = gpu_submit_cmd(cmd, sizeof(*cmd), resp, sizeof(*resp));
    if (ret < 0 || resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_printf("virtio_gpu: SET_SCANOUT failed (type=0x%x)\n",
                      resp->type);
        kfree(cmd);
        kfree(resp);
        return -1;
    }

    serial_printf("virtio_gpu: scanout %d set to resource %d (%dx%d)\n",
                  scanout_id, resource_id, width, height);
    kfree(cmd);
    kfree(resp);
    return 0;
}

static int gpu_transfer_to_host(uint32_t resource_id,
                                 uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height) {
    memset(&virtio_xfer_cmd, 0, sizeof(virtio_xfer_cmd));
    memset(&virtio_xfer_resp, 0, sizeof(virtio_xfer_resp));
    virtio_xfer_cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    virtio_xfer_cmd.r.x = x;
    virtio_xfer_cmd.r.y = y;
    virtio_xfer_cmd.r.width = width;
    virtio_xfer_cmd.r.height = height;
    virtio_xfer_cmd.offset = (uint64_t)y * gpu.pitch + (uint64_t)x * 4;
    virtio_xfer_cmd.resource_id = resource_id;

    return gpu_submit_cmd(&virtio_xfer_cmd, sizeof(virtio_xfer_cmd),
                          &virtio_xfer_resp, sizeof(virtio_xfer_resp));
}

static int gpu_resource_flush(uint32_t resource_id,
                               uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height) {
    memset(&virtio_flush_cmd, 0, sizeof(virtio_flush_cmd));
    memset(&virtio_flush_resp, 0, sizeof(virtio_flush_resp));
    virtio_flush_cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    virtio_flush_cmd.r.x = x;
    virtio_flush_cmd.r.y = y;
    virtio_flush_cmd.r.width = width;
    virtio_flush_cmd.r.height = height;
    virtio_flush_cmd.resource_id = resource_id;

    return gpu_submit_cmd(&virtio_flush_cmd, sizeof(virtio_flush_cmd),
                          &virtio_flush_resp, sizeof(virtio_flush_resp));
}

/* Enable virtio scanout on first present — keeps Limine/VGA visible during
 * driver init so loading the GPU driver does not blank the QEMU window. */
static int virtio_gpu_ensure_scanout(void) {
    if (!gpu.initialized) return -1;
    if (gpu.display_attached) return 0;

    if (!gpu.owns_framebuffer) {
        /* virtio-vga: Limine FB lives in the device BAR and is already the
         * live VGA surface.  SET_SCANOUT to a resource over the same RAM
         * blanks QEMU (see serial.log ~0.85s).  Leave VGA scanout alone. */
        gpu.display_attached = true;
        serial_puts("virtio_gpu: using live VGA scanout (skipped SET_SCANOUT)\n");
        return 0;
    }

    if (gpu_set_scanout(0, gpu.resource_id, gpu.width, gpu.height) < 0)
        return -1;
    gpu.display_attached = true;
    serial_puts("virtio_gpu: scanout enabled\n");
    return 0;
}

bool virtio_gpu_using_live_vga(void) {
    return gpu.initialized && !gpu.owns_framebuffer;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int virtio_gpu_init(void) {
    memset(&gpu, 0, sizeof(gpu));

    /* Scan PCI for virtio-GPU device.
     * QEMU uses vendor 0x1AF4 and device IDs:
     *   0x1050 = virtio-gpu (modern/non-transitional)
     *   0x1040 = virtio-gpu (transitional, needs subsystem check)
     */
    struct pci_device *pci = NULL;

    /* Try modern device ID first */
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = pci_devices[i];
        if (dev->vendor_id == VIRTIO_PCI_VENDOR_ID) {
            /* Check for GPU: class 0x03 (display) or device ID 0x1050 */
            if (dev->device_id == VIRTIO_PCI_MODERN_GPU ||
                (dev->class_code == 0x03 && dev->subclass_code == 0x00)) {
                pci = dev;
                break;
            }
        }
    }

    if (!pci) {
        serial_puts("virtio_gpu: no virtio-GPU device found\n");
        return -1;
    }

    printf("Virtio-GPU: Found device at PCI %02x:%02x.%x\n",
           pci->bus, pci->device, pci->function);

    /* Initialize virtio transport */
    if (virtio_init_device(&gpu.vdev, pci) < 0) {
        printf("Virtio-GPU: Failed to initialize virtio transport\n");
        return -1;
    }

    /* Negotiate features (we don't need virgl for basic 2D) */
    uint64_t features = virtio_negotiate_features(&gpu.vdev,
        (1ULL << VIRTIO_GPU_F_EDID));

    (void)features;

    /* Set up controlq (queue 0) */
    if (virtio_setup_queue(&gpu.vdev, 0, 0) < 0) {
        printf("Virtio-GPU: Failed to set up controlq\n");
        return -1;
    }

    /* Set up cursorq (queue 1) if available */
    if (gpu.vdev.num_queues > 1) {
        virtio_setup_queue(&gpu.vdev, 1, 0);
    }

    /* Mark device as ready */
    virtio_device_ready(&gpu.vdev);

    /* Read device configuration */
    if (gpu.vdev.device_cfg) {
        volatile virtio_gpu_config_t *cfg =
            (volatile virtio_gpu_config_t *)gpu.vdev.device_cfg;
        gpu.num_scanouts = cfg->num_scanouts;
        serial_printf("virtio_gpu: %d scanouts available\n", gpu.num_scanouts);
    }

    /* Get display info from the device */
    if (gpu_get_display_info() < 0) {
        printf("Virtio-GPU: Failed to get display info, using defaults\n");
        gpu.width = DEFAULT_WIDTH;
        gpu.height = DEFAULT_HEIGHT;
    }

    /*
     * Use Limine's linear framebuffer size as the guest drawing resolution.
     * QEMU often reports a small default scanout (e.g. 640x480) while Limine
     * was configured at the real boot resolution (e.g. 1280x800).  SDL and
     * fb_get_* use Limine dimensions; if the virtio resource stayed smaller,
     * sdl_sync_to_hal would clip to the GPU buffer and the picture looked
     * wrongly cropped or scaled.
     */
    if (fb && fb->width > 0 && fb->height > 0 &&
        (gpu.width != (uint32_t)fb->width ||
         gpu.height != (uint32_t)fb->height)) {
        serial_printf(
            "virtio_gpu: matching resource to Limine FB %ux%u (scanout was %ux%u)\n",
            (uint32_t)fb->width, (uint32_t)fb->height, gpu.width, gpu.height);
        gpu.width  = (uint32_t)fb->width;
        gpu.height = (uint32_t)fb->height;
    }

    /* Backing store: alias the Limine linear FB when available so console
     * pixels and virtio scanout share one buffer (no black handoff). */
    gpu.pitch = gpu.width * 4;
    if (fb && fb->address && fb->bpp == 32 && fb->pitch >= gpu.width * 4) {
        gpu.framebuffer = (uint32_t *)fb->address;
        gpu.fb_phys = (uint64_t)fb->address - vmm_higher_half_offset;
        gpu.pitch = (uint32_t)fb->pitch;
        gpu.fb_size = gpu.pitch * gpu.height;
        gpu.owns_framebuffer = false;
        serial_printf("virtio_gpu: using Limine FB backing (phys 0x%llx pitch %u)\n",
                      (unsigned long long)gpu.fb_phys, gpu.pitch);
    } else {
        gpu.fb_size = gpu.width * gpu.height * 4;
        gpu.framebuffer = (uint32_t *)kalloc(gpu.fb_size);
        if (!gpu.framebuffer) {
            printf("Virtio-GPU: Failed to allocate framebuffer (%d bytes)\n",
                   gpu.fb_size);
            return -1;
        }
        memset(gpu.framebuffer, 0, gpu.fb_size);
        gpu.fb_phys = (uint64_t)gpu.framebuffer - vmm_higher_half_offset;
        gpu.owns_framebuffer = true;
    }

    /* Create a 2D resource */
    gpu.resource_id = next_resource_id++;
    if (gpu_create_resource(gpu.resource_id, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
                            gpu.width, gpu.height) < 0) {
        printf("Virtio-GPU: Failed to create resource\n");
        if (gpu.owns_framebuffer) kfree(gpu.framebuffer);
        return -1;
    }

    /* Attach framebuffer backing store */
    if (gpu_attach_backing(gpu.resource_id, gpu.fb_phys, gpu.fb_size) < 0) {
        printf("Virtio-GPU: Failed to attach backing store\n");
        if (gpu.owns_framebuffer) kfree(gpu.framebuffer);
        return -1;
    }

    /* Driver is ready; defer set_scanout until the first flush/present so
     * Limine/VGA keeps driving the display through early boot. */
    gpu.initialized = true;
    gpu.display_attached = false;

    printf("Virtio-GPU: Initialized %dx%d display (resource %d)\n",
           gpu.width, gpu.height, gpu.resource_id);

    return 0;
}

bool virtio_gpu_is_available(void) {
    return gpu.initialized;
}

uint32_t* virtio_gpu_get_framebuffer(void) {
    return gpu.framebuffer;
}

uint32_t virtio_gpu_get_width(void) {
    return gpu.width;
}

uint32_t virtio_gpu_get_height(void) {
    return gpu.height;
}

uint32_t virtio_gpu_get_pitch(void) {
    return gpu.pitch ? gpu.pitch : gpu.width * 4;
}

void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!gpu.initialized) return;
    if (virtio_gpu_ensure_scanout() < 0) return;
    /* Live VGA: pixels are already visible; virtio transfer/flush not needed. */
    if (!gpu.owns_framebuffer) return;

    /* Clamp to display bounds */
    if (x + width > gpu.width)   width = gpu.width - x;
    if (y + height > gpu.height) height = gpu.height - y;

    /* Transfer from host memory to GPU resource */
    gpu_transfer_to_host(gpu.resource_id, x, y, width, height);

    /* Flush the resource to the display */
    gpu_resource_flush(gpu.resource_id, x, y, width, height);
}

void virtio_gpu_flush_all(void) {
    virtio_gpu_flush(0, 0, gpu.width, gpu.height);
}

void virtio_gpu_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t color) {
    if (!gpu.framebuffer) return;

    /* Clamp to framebuffer bounds */
    if (x + w > gpu.width)   w = gpu.width - x;
    if (y + h > gpu.height)  h = gpu.height - y;

    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *pixel = (uint32_t *)((uint8_t *)gpu.framebuffer
                                        + (uint64_t)row * gpu.pitch + x * 4);
        for (uint32_t col = 0; col < w; col++) {
            pixel[col] = color;
        }
    }
}

void virtio_gpu_clear(uint32_t color) {
    if (!gpu.framebuffer) return;

    for (uint32_t row = 0; row < gpu.height; row++) {
        uint32_t *line = (uint32_t *)((uint8_t *)gpu.framebuffer
                                      + (uint64_t)row * gpu.pitch);
        for (uint32_t col = 0; col < gpu.width; col++) {
            line[col] = color;
        }
    }
}

int virtio_gpu_set_viewport_size(uint32_t width, uint32_t height)
{
    if (!gpu.initialized)
        return -1;
    if (width == 0 || height == 0)
        return -1;
    if (width == gpu.width && height == gpu.height)
        return 0;

    (void)gpu_resource_detach_backing(gpu.resource_id);
    if (gpu_resource_unref_id(gpu.resource_id) < 0)
        return -1;

    if (gpu.owns_framebuffer)
        kfree(gpu.framebuffer);
    gpu.framebuffer = NULL;
    gpu.owns_framebuffer = true;

    gpu.width  = width;
    gpu.height = height;
    gpu.pitch  = width * 4;
    gpu.fb_size = width * height * 4u;
    gpu.framebuffer = (uint32_t *)kalloc(gpu.fb_size);
    if (!gpu.framebuffer) {
        serial_puts("virtio_gpu: set_viewport_size: framebuffer alloc failed\n");
        gpu.initialized = false;
        gpu.display_attached = false;
        return -1;
    }
    memset(gpu.framebuffer, 0, gpu.fb_size);
    gpu.fb_phys = (uint64_t)gpu.framebuffer - vmm_higher_half_offset;

    if (gpu_create_resource(gpu.resource_id, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
                            width, height) < 0) {
        kfree(gpu.framebuffer);
        gpu.framebuffer = NULL;
        return -1;
    }
    if (gpu_attach_backing(gpu.resource_id, gpu.fb_phys, gpu.fb_size) < 0) {
        kfree(gpu.framebuffer);
        gpu.framebuffer = NULL;
        return -1;
    }
    if (gpu_set_scanout(0, gpu.resource_id, width, height) < 0) {
        kfree(gpu.framebuffer);
        gpu.framebuffer = NULL;
        return -1;
    }

    gpu_hal_sync_active_virtio_surface();
    virtio_gpu_clear(0);
    virtio_gpu_flush_all();
    serial_printf("virtio_gpu: viewport resized to %ux%u\n", width, height);
    return 0;
}

/* Shared legacy INTx: virtio-gpu raises a level IRQ on queue completion even
 * though this driver polls. Reading the ISR register deasserts the line.
 * Called from other handlers on the same line. Returns the ISR bits read. */
uint8_t virtio_gpu_ack_irq(void) {
    if (!gpu.vdev.isr_cfg) return 0;
    return *gpu.vdev.isr_cfg;
}
