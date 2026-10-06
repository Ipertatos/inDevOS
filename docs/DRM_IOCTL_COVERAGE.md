# DRM Ioctl Coverage Matrix

**Pinned Linux uAPI version: 6.6 LTS**

This document tracks which Linux DRM ioctls are implemented (or stubbed) in inDevOS, per driver backend.  Mesa drivers (via libdrm) expect these ioctls to exist on `/dev/dri/card*` and `/dev/dri/renderD*` nodes.

Grow this table as each Mesa driver is brought up.

---

## Legend

| Status | Meaning |
|--------|---------|
| **DONE** | Fully handled, returns correct data |
| **STUB** | Handler exists, returns fixed/minimal data |
| **TODO** | Not yet implemented, required for the target Mesa driver |
| **N/A** | Not needed for that backend |

---

## Common DRM ioctls (`drm.h`)

| Ioctl | Nr | inDevOS handler | virtio-gpu | amdgpu | Notes |
|-------|----|----------------|------------|--------|-------|
| `DRM_IOCTL_VERSION` | 0x00 | **DONE** | DONE | DONE | Returns `indevos` driver name, v0.1.0 |
| `DRM_IOCTL_GET_CAP` | 0x0C | **DONE** | DONE | DONE | DUMB_BUFFER=1, depth=32, SYNCOBJ=0 |
| `DRM_IOCTL_SET_CLIENT_CAP` | 0x0D | TODO | TODO | TODO | Needed for KMS atomic |
| `DRM_IOCTL_GEM_CLOSE` | 0x09 | TODO | TODO | TODO | Required once GEM is live |
| `DRM_IOCTL_GEM_OPEN` | 0x0B | TODO | TODO | TODO | Legacy GEM name sharing |
| `DRM_IOCTL_GEM_FLINK` | 0x0A | TODO | TODO | TODO | Legacy |

---

## GEM / mmap ioctls

| Ioctl | Nr | Status | Notes |
|-------|----|--------|-------|
| `DRM_IOCTL_MODE_CREATE_DUMB` | 0xB2 | TODO | Dumb buffer creation (KMS path) |
| `DRM_IOCTL_MODE_MAP_DUMB` | 0xB3 | TODO | mmap offset for dumb buffers |
| `DRM_IOCTL_MODE_DESTROY_DUMB` | 0xB4 | TODO | |

---

## Sync objects

| Ioctl | Nr | Status | Notes |
|-------|----|--------|-------|
| `DRM_IOCTL_SYNCOBJ_CREATE` | 0xBF | TODO | Binary/timeline sync objects |
| `DRM_IOCTL_SYNCOBJ_DESTROY` | 0xC0 | TODO | |
| `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` | 0xC1 | TODO | |
| `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` | 0xC2 | TODO | |
| `DRM_IOCTL_SYNCOBJ_WAIT` | 0xC3 | TODO | |
| `DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT` | 0xCA | TODO | Vulkan timeline semaphores |

---

## virtio-gpu specific (`virtgpu_drm.h`)

Mesa winsys: `src/virtio/vulkan/` and `src/gallium/winsys/virgl/drm/`

| Ioctl | Nr | Status | Notes |
|-------|----|--------|-------|
| `DRM_VIRTGPU_MAP` | 0x01 | TODO | Map resource to CPU |
| `DRM_VIRTGPU_EXECBUFFER` | 0x02 | TODO | Submit 3D commands |
| `DRM_VIRTGPU_GETPARAM` | 0x03 | TODO | Query device params |
| `DRM_VIRTGPU_RESOURCE_CREATE` | 0x04 | TODO | Create 2D/3D resource |
| `DRM_VIRTGPU_RESOURCE_INFO` | 0x05 | TODO | Query resource metadata |
| `DRM_VIRTGPU_TRANSFER_FROM_HOST` | 0x06 | TODO | |
| `DRM_VIRTGPU_TRANSFER_TO_HOST` | 0x07 | TODO | |
| `DRM_VIRTGPU_WAIT` | 0x08 | TODO | Fence wait |
| `DRM_VIRTGPU_GET_CAPS` | 0x09 | TODO | VIRGL/Venus capset query |
| `DRM_VIRTGPU_RESOURCE_CREATE_BLOB` | 0x0A | TODO | Blob resources (Venus) |
| `DRM_VIRTGPU_CONTEXT_INIT` | 0x0B | TODO | |

---

## amdgpu specific (`amdgpu_drm.h`)

Mesa winsys: `src/amd/vulkan/winsys/amdgpu/` and `src/gallium/winsys/amdgpu/drm/`

| Ioctl | Nr | Status | Notes |
|-------|----|--------|-------|
| `DRM_AMDGPU_GEM_CREATE` | 0x00 | TODO | BO allocation |
| `DRM_AMDGPU_GEM_MMAP` | 0x01 | TODO | mmap offset |
| `DRM_AMDGPU_CTX` | 0x02 | TODO | Context create/destroy |
| `DRM_AMDGPU_BO_LIST` | 0x03 | TODO | |
| `DRM_AMDGPU_CS` | 0x04 | TODO | Command submission |
| `DRM_AMDGPU_INFO` | 0x05 | TODO | Device/firmware queries (huge) |
| `DRM_AMDGPU_GEM_METADATA` | 0x06 | TODO | |
| `DRM_AMDGPU_GEM_WAIT_IDLE` | 0x07 | TODO | |
| `DRM_AMDGPU_GEM_VA` | 0x08 | TODO | Virtual address map/unmap |
| `DRM_AMDGPU_WAIT_CS` | 0x09 | TODO | |
| `DRM_AMDGPU_GEM_OP` | 0x10 | TODO | |
| `DRM_AMDGPU_GEM_USERPTR` | 0x11 | TODO | |
| `DRM_AMDGPU_WAIT_FENCES` | 0x12 | TODO | |
| `DRM_AMDGPU_VM` | 0x13 | TODO | |
| `DRM_AMDGPU_FENCE_TO_HANDLE` | 0x14 | TODO | |
| `DRM_AMDGPU_SCHED` | 0x15 | TODO | |

---

## KMS / mode-setting (for `card0`, not `renderD*`)

Not yet targeted.  Mesa render-only paths (`renderD128`) do not require KMS.

| Ioctl | Nr | Status | Notes |
|-------|----|--------|-------|
| `DRM_IOCTL_MODE_GETRESOURCES` | 0xA0 | TODO | |
| `DRM_IOCTL_MODE_GETCONNECTOR` | 0xA7 | TODO | |
| `DRM_IOCTL_MODE_GETENCODER` | 0xA6 | TODO | |
| `DRM_IOCTL_MODE_GETCRTC` | 0xA1 | TODO | |
| `DRM_IOCTL_MODE_SETCRTC` | 0xA2 | TODO | |
| `DRM_IOCTL_MODE_ADDFB2` | 0xB8 | TODO | |
| `DRM_IOCTL_MODE_RMFB` | 0xAF | TODO | |
| `DRM_IOCTL_MODE_PAGE_FLIP` | 0xB0 | TODO | |
| `DRM_IOCTL_MODE_ATOMIC` | 0xBC | TODO | Atomic KMS |

---

## Revision

- **2026-03-23:** Initial matrix.  `VERSION` + `GET_CAP` implemented; everything else TODO.
