/**
 * @file drm.h
 * @brief Linux DRM ioctl compatibility layer (subset)
 *
 * Implements the minimum DRM uAPI surface that libdrm / Mesa need to
 * open a render node and query basic capabilities.  Struct layouts and
 * ioctl numbers are copied from Linux 6.6 LTS headers (include/uapi/
 * drm/drm.h, drm_mode.h) so that an eventual libdrm port can issue
 * the same ioctls without translation.
 *
 * Pin: Linux 6.6 LTS uAPI
 *
 * Today only DRM_IOCTL_VERSION and DRM_IOCTL_GET_CAP are handled.
 * See docs/DRM_IOCTL_COVERAGE.md for the full status matrix.
 */

#ifndef DRM_H
#define DRM_H

#include "types.h"

/* ============================================================================
 * Linux ioctl encoding (compatible with _IO/_IOR/_IOW/_IOWR macros)
 * ============================================================================ */

#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT   + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE   0U
#define _IOC_WRITE  1U
#define _IOC_READ   2U

#define _IOC(dir, type, nr, size) \
    (((dir)  << _IOC_DIRSHIFT)  | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT)   | \
     ((size) << _IOC_SIZESHIFT))

#define _IO(type, nr)        _IOC(_IOC_NONE,  type, nr, 0)
#define _IOR(type, nr, sz)   _IOC(_IOC_READ,  type, nr, sizeof(sz))
#define _IOW(type, nr, sz)   _IOC(_IOC_WRITE, type, nr, sizeof(sz))
#define _IOWR(type, nr, sz)  _IOC(_IOC_READ | _IOC_WRITE, type, nr, sizeof(sz))

/* Extract fields from an ioctl number */
#define _IOC_NR(cmd)    (((cmd) >> _IOC_NRSHIFT)   & ((1 << _IOC_NRBITS)  - 1))
#define _IOC_TYPE(cmd)  (((cmd) >> _IOC_TYPESHIFT)  & ((1 << _IOC_TYPEBITS) - 1))
#define _IOC_SIZE(cmd)  (((cmd) >> _IOC_SIZESHIFT)  & ((1 << _IOC_SIZEBITS) - 1))
#define _IOC_DIR(cmd)   (((cmd) >> _IOC_DIRSHIFT)   & ((1 << _IOC_DIRBITS)  - 1))

/* DRM ioctl base type character (= 'd' in Linux) */
#define DRM_IOCTL_BASE  'd'

/* ============================================================================
 * DRM ioctl numbers  (Linux 6.6, include/uapi/drm/drm.h)
 * Only the ones we handle or plan to handle are listed here.
 * ============================================================================ */

#define DRM_IOCTL_VERSION           _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP           _IOWR(DRM_IOCTL_BASE, 0x0C, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP    _IOW(DRM_IOCTL_BASE,  0x0D, struct drm_set_client_cap)

/* GEM stubs (future) */
#define DRM_IOCTL_GEM_CLOSE         _IOW(DRM_IOCTL_BASE,  0x09, struct drm_gem_close)
#define DRM_IOCTL_GEM_OPEN          _IOWR(DRM_IOCTL_BASE, 0x0B, struct drm_gem_open)

/* ============================================================================
 * DRM capability tokens  (DRM_CAP_*)
 * ============================================================================ */

#define DRM_CAP_DUMB_BUFFER             0x1
#define DRM_CAP_VBLANK_HIGH_CRTC        0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH    0x3
#define DRM_CAP_DUMB_PREFER_SHADOW      0x4
#define DRM_CAP_PRIME                   0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC     0x6
#define DRM_CAP_ASYNC_PAGE_FLIP         0x7
#define DRM_CAP_CURSOR_WIDTH            0x8
#define DRM_CAP_CURSOR_HEIGHT           0x9
#define DRM_CAP_ADDFB2_MODIFIERS        0x10
#define DRM_CAP_PAGE_FLIP_TARGET        0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT    0x12
#define DRM_CAP_SYNCOBJ                 0x13
#define DRM_CAP_SYNCOBJ_TIMELINE        0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP  0x15

/* PRIME flags */
#define DRM_PRIME_CAP_IMPORT  0x1
#define DRM_PRIME_CAP_EXPORT  0x2

/* ============================================================================
 * DRM structures  (Linux-compatible layouts)
 * ============================================================================ */

struct drm_version {
    int32_t  version_major;
    int32_t  version_minor;
    int32_t  version_patchlevel;
    uint32_t name_len;
    char    *name;
    uint32_t date_len;
    char    *date;
    uint32_t desc_len;
    char    *desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_gem_open {
    uint32_t name;
    uint32_t handle;
    uint64_t size;
};

/* ============================================================================
 * DRM minor types  (device node numbering)
 * ============================================================================ */

#define DRM_MINOR_PRIMARY   0   /* /dev/dri/card0   */
#define DRM_MINOR_RENDER  128   /* /dev/dri/renderD128 */

/* ============================================================================
 * Kernel-side API
 * ============================================================================ */

/**
 * @brief Initialize the DRM subsystem
 *
 * Registers /dev/dri/ device nodes with the VFS (if available) and
 * prepares ioctl dispatch tables.
 */
void drm_init(void);

/**
 * @brief Handle a DRM ioctl
 *
 * @param minor  DRM minor number (DRM_MINOR_PRIMARY or DRM_MINOR_RENDER)
 * @param cmd    Encoded ioctl command (DRM_IOCTL_*)
 * @param arg    Pointer to the ioctl argument struct
 * @return 0 on success, negative errno on failure
 */
int drm_ioctl(int minor, uint32_t cmd, void *arg);

#endif /* DRM_H */
