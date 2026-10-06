/**
 * @file drm.c
 * @brief Linux DRM ioctl compatibility — stub handlers
 *
 * Provides DRM_IOCTL_VERSION and DRM_IOCTL_GET_CAP so that a future
 * libdrm port can successfully open /dev/dri/renderD128 and query the
 * driver name and basic capabilities.
 *
 * Today these are kernel-callable stubs.  Once the VFS gains an ioctl
 * op (or a dedicated DRM syscall is added), userspace access will be
 * wired through the same dispatch function.
 *
 * Pin: Linux 6.6 LTS uAPI
 */

#include "drm.h"
#include "gpu_hal.h"
#include "mem.h"
#include "string.h"
#include <stddef.h>

extern int dev_dri_vfs_mount(void);

extern int  printf(const char *fmt, ...);
extern void serial_printf(const char *fmt, ...);

/* ============================================================================
 * Version info  (returned by DRM_IOCTL_VERSION)
 * ============================================================================ */

#define INDEVOS_DRM_MAJOR   0
#define INDEVOS_DRM_MINOR   1
#define INDEVOS_DRM_PATCH   0

static const char drm_driver_name[] = "indevos";
static const char drm_driver_date[] = "20260323";
static const char drm_driver_desc[] = "inDevOS DRM (GPU HAL bridge)";

/* ============================================================================
 * Helpers
 * ============================================================================ */

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void copy_str_field(char *dst, uint32_t *dst_len, const char *src)
{
    uint32_t slen = (uint32_t)strlen(src);
    if (dst && *dst_len > 0) {
        uint32_t n = min_u32(*dst_len, slen);
        memcpy(dst, src, n);
    }
    *dst_len = slen;
}

/* ============================================================================
 * Ioctl handlers
 * ============================================================================ */

static int drm_ioctl_version(void *arg)
{
    struct drm_version *v = (struct drm_version *)arg;
    if (!v) return -1;

    v->version_major      = INDEVOS_DRM_MAJOR;
    v->version_minor      = INDEVOS_DRM_MINOR;
    v->version_patchlevel = INDEVOS_DRM_PATCH;

    copy_str_field(v->name, &v->name_len, drm_driver_name);
    copy_str_field(v->date, &v->date_len, drm_driver_date);
    copy_str_field(v->desc, &v->desc_len, drm_driver_desc);

    return 0;
}

static int drm_ioctl_get_cap(void *arg)
{
    struct drm_get_cap *cap = (struct drm_get_cap *)arg;
    if (!cap) return -1;

    switch (cap->capability) {
    case DRM_CAP_DUMB_BUFFER:
        cap->value = 0;  /* No GEM / dumb-buffer allocator yet */
        break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:
        cap->value = 32;
        break;
    case DRM_CAP_DUMB_PREFER_SHADOW:
        cap->value = 0;
        break;
    case DRM_CAP_PRIME:
        cap->value = 0;  /* No PRIME import/export yet */
        break;
    case DRM_CAP_TIMESTAMP_MONOTONIC:
        cap->value = 1;
        break;
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT:
        cap->value = 64;
        break;
    case DRM_CAP_SYNCOBJ:
    case DRM_CAP_SYNCOBJ_TIMELINE:
        cap->value = 0;  /* Not yet */
        break;
    default:
        cap->value = 0;
        return -22;  /* -EINVAL */
    }

    return 0;
}

/* ============================================================================
 * Dispatch
 * ============================================================================ */

int drm_ioctl(int minor, uint32_t cmd, void *arg)
{
    (void)minor;

    if (cmd == DRM_IOCTL_VERSION)
        return drm_ioctl_version(arg);

    if (cmd == DRM_IOCTL_GET_CAP)
        return drm_ioctl_get_cap(arg);

    if (cmd == DRM_IOCTL_GEM_CLOSE) {
        /* No GEM handles allocated by the kernel yet; accept close as no-op */
        return 0;
    }

    if (cmd == DRM_IOCTL_GEM_OPEN) {
        return -22;
    }

    serial_printf("[drm] unhandled ioctl nr=0x%x type='%c'\n",
                  _IOC_NR(cmd), (char)_IOC_TYPE(cmd));
    return -22;  /* -EINVAL */
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

void drm_init(void)
{
    if (dev_dri_vfs_mount() != 0) {
        serial_printf("[drm] warning: could not mount /dev/dri (VFS)\n");
    }

    serial_printf("[drm] DRM subsystem initialized (driver: %s v%d.%d.%d)\n",
                  drm_driver_name, INDEVOS_DRM_MAJOR, INDEVOS_DRM_MINOR,
                  INDEVOS_DRM_PATCH);
    printf("DRM: driver=%s v%d.%d.%d  nodes=/dev/dri/card0,renderD128 (stub)\n",
           drm_driver_name, INDEVOS_DRM_MAJOR, INDEVOS_DRM_MINOR,
           INDEVOS_DRM_PATCH);

    /* Self-test: verify VERSION and GET_CAP work */
    struct drm_version ver;
    memset(&ver, 0, sizeof(ver));
    if (drm_ioctl(DRM_MINOR_RENDER, DRM_IOCTL_VERSION, &ver) == 0) {
        serial_printf("[drm] self-test VERSION: %d.%d.%d name_len=%u\n",
                      ver.version_major, ver.version_minor,
                      ver.version_patchlevel, ver.name_len);
    }

    struct drm_get_cap cap;
    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_DUMB_BUFFER;
    if (drm_ioctl(DRM_MINOR_RENDER, DRM_IOCTL_GET_CAP, &cap) == 0) {
        serial_printf("[drm] self-test GET_CAP(DUMB_BUFFER)=%u\n",
                      (uint32_t)cap.value);
    }
}
