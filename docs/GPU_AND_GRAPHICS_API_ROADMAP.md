# GPU, drivers, and graphics API roadmap (inDevOS)

This document extends the SDL/display plan toward **multiple drivers** and future **OpenGL / Vulkan**-class APIs without painting the project into a corner.

It is explicitly **Mesa-oriented**: the endgame is to reuse Mesa’s Vulkan/OpenGL drivers (and LLVM-based compilers where required) by presenting a **DRM-compatible userspace ABI** from the kernel, so porting effort stays in the **KMD + a few winsys tweaks**, not a rewrite of Mesa’s compiler stack.

---

## Design principles

1. **Separate concerns (layers)**  
   Do not tie “what users draw” directly to “how one driver pushes pixels.” Future GL/Vulkan need stable boundaries between:
   - **Hardware access** (MMIO, IRQ, DMA, fence semantics)
   - **Memory & submission** (buffers, command streams, synchronization)
   - **API implementation** (state machine, shaders, pipelines — mostly userspace in modern OSes)

2. **DRM-shaped kernel UAPI for Mesa (compatibility goal)**  
   Mesa drivers do not call your custom syscalls directly; they go through **`libdrm`** and **`drmIoctl()`** against device nodes such as **`/dev/dri/card0`** (primary: display + legacy) and **`/dev/dri/renderD128`** (render-only: GPGPU/GL/Vulkan without modesetting).  
   **Target:** implement the **subset of Linux DRM ioctls** each chosen Mesa **winsys** needs, with matching **ioctl numbers and struct layouts** (use Linux `drm.h` / `drm_fourcc.h` / driver-specific headers as the spec). Your in-kernel HAL then **implements** those ioctls on top of real hardware or virtio.

3. **Prefer Vulkan-shaped internals even if you ship OpenGL first**  
   Vulkan maps cleanly to explicit queues, memory types, and pipelines. OpenGL can be layered (e.g. Zink on Vulkan) later; the reverse is harder.

4. **Kernel stays small; put heavy API logic in userspace (Mesa)**  
   Typical pattern: kernel = DRM ioctl surface + IRQ/fences + buffer lifecycle; userspace = **Mesa** (NIR, state trackers, Vulkan/OpenGL drivers).

5. **One abstraction, many backends**  
   All paths (Limine FB, virtio-gpu 2D, VMware SVGA, AMD, future Intel/NVIDIA) should plug into the **same** internal interfaces **behind** the DRM ioctl layer so SDL, the compositor, and Mesa share one KMD story.

---

## Target architecture (conceptual)

```
┌─────────────────────────────────────────────────────────────┐
│  Applications: SDL shim, games, toolkits                     │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  Future: Vulkan API / OpenGL (optional Zink on Vulkan)        │
│  Today: 2D present (blit, texture upload, swap)             │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  Mesa (userspace): Vulkan/OpenGL/CL + NIR + LLVM (per driver) │
│  libdrm: drmIoctl → /dev/dri/card* , renderD*                 │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  Optional: Vulkan loader (Khronos ICD) — Mesa exports .so     │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  Kernel: Linux-compatible DRM ioctl layer (“inDevOS DRM”)    │
│  - File ops on render + primary nodes                         │
│  - GEM / driver ioctls → your HAL (virtio_gpu, amdgpu, …)     │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  HAL / KMD backends: Limine FB, virtio-gpu, SVGA, AMD, …      │
└─────────────────────────────────────────────────────────────┘
```

---

## Mesa compatibility (porting Mesa for performance)

### How Mesa expects to talk to the kernel

| Piece | Role |
|--------|------|
| **Winsys** | Per-driver glue in Mesa (`src/{driver}/vulkan/*_winsys*.c`, `src/gallium/winsys/...`) that opens DRM nodes and issues ioctls. |
| **libdrm** | Thin wrappers around `ioctl()`; many ioctls are **driver-specific** (e.g. `amdgpu_drm.h`, `virtgpu_drm.h`). |
| **GEM / driver BO** | Buffer create, map, mmap offset, export/import (later); command submission where applicable. |
| **Sync** | `DRM_SYNCOBJ_*`, timelines, or driver-specific fences — needed for correct Vulkan/GL. |

**Implication:** Your “GPU syscalls” are ideally **`ioctl` on a DRM character device**, not a bespoke syscall table, if you want **upstream Mesa or near-upstream** with only **OS port patches** (paths, feature ifdefs, missing uAPI stubs).

### Compatibility strategies (pick one primary path)

1. **Recommended — DRM ioctl compatibility layer**  
   - Add `/dev/dri/card0` + `/dev/dri/renderD128` (or Linux-like minor numbering).  
   - Implement **ioctl handlers** that decode the same structs/numbers as Linux for the **subset** your first Mesa driver uses.  
   - Port or cross-build **libdrm** + Mesa with `meson` targeting inDevOS (new OS triple or `__indevos__` with Linux-like defines).  
   - **Pros:** Maximum reuse of Mesa; performance is Mesa’s codegen + your KMD.  
   - **Cons:** You must track ioctl stability and driver expectations.

2. **Alternative — inDevOS-specific winsys in Mesa**  
   Keep a small Mesa fork that replaces only the winsys with calls into your syscalls.  
   - **Pros:** No need to mimic every DRM ioctl at first.  
   - **Cons:** Ongoing merge pain; harder to pull Mesa updates.

3. **Not recommended — “implement OpenGL in kernel”**  
   Conflicts with Mesa’s model and long-term maintenance.

### Sensible Mesa bring-up order (performance vs effort)

| Order | Mesa target | Why |
|-------|-------------|-----|
| 1 | **Lavapipe** (`vulkan_lvp`) | Software Vulkan on CPU; validates **Vulkan loader + ICD + windowing** without correct GPU UAPI. Optional for CI. |
| 2 | **VirtIO GPU (Venus / virgl)** | If your kernel exposes **virtio-gpu DRM UAPI** + **VIRGL** (and/or Venus for Vulkan), Mesa already has drivers; good **QEMU performance** path before bare metal. |
| 3 | **AMD / Radeon** (`vulkan_radeon`, `radeonsi` via amdgpu) | Best **bare-metal performance** target for your hardware notes (`ref/`), but needs **large AMDGPU ioctl + GEM + scheduler** parity. |

Start with (2) for GPU-accelerated development in VMs; converge (3) for real silicon.

### IOCTL / UAPI checklist (grow as you enable each Mesa driver)

Use the **Linux headers** for the exact layouts (do not invent parallel structs):

- **Common DRM:** `DRM_IOCTL_VERSION`, `GET_CAP`, `SET_CLIENT_CAP`, mode-setting ioctls if you use `card0` for KMS.  
- **GEM (generic):** `DRM_IOCTL_GEM_OPEN`, `GEM_CLOSE`, `GEM_FLINK` (legacy), **mmap offset** handling.  
- **virtgpu:** `virtgpu_drm.h` — resource create, map, submit, context (align with your `virtio_gpu.c` evolution).  
- **amdgpu:** `amdgpu_drm.h` — `AMDGPU_GEM_*`, `AMDGPU_CS`, `AMDGPU_INFO`, VM, fences (large surface).  
- **Sync objects:** Vulkan needs `DRM_SYNCOBJ_*` or equivalent semantics for timeline/binary fences.

Document in code **which Linux version** you pin ioctl layouts to (e.g. **6.6 or 6.12 LTS**).

### Build / runtime requirements for Mesa on inDevOS

- **Toolchain:** C11/C++17 (check Mesa release), **Meson**, Ninja; many drivers need **LLVM** as a dependency for shader compilation.  
- **POSIX + libc:** `mmap`, `pthread`, `dlopen` (or static link), enough for ICD loading.  
- **Page size / DMA:** Mesa assumes coherent mmap of BOs; your GEM implementation must provide **CPU-visible mappings** where the winsys expects them.  
- **PCI / device enumeration:** Mesa winsys often opens DRM by path; firmware must expose stable **`/dev/dri/*`** names.

### Mesa patches you should plan for (even with DRM parity)

- **OS define:** `__indevos__` for small `#ifdef` blocks (paths, optional syscalls, missing `epoll` shims).  
- **Loader:** ICD path `/usr/lib/libvulkan_*` or in-tree.  
- **EGL/WSI (optional):** If you skip X11/Wayland, you may use **headless**, **surfaceless**, or a **tiny inDevOS WSI** extension for swapchain → your compositor.

---

## Phase A — Driver model & HAL (foundation)

**Goal:** Any new driver implements the same hooks; SDL only talks to “the active display/GPU device.”

| Component | Responsibility |
|-----------|------------------|
| `gpu_device` / `display_device` | Probe order, capabilities bitmask (2D only, 3D, compute later) |
| **Surface / buffer** | CPU-visible pixel buffer or device memory handle (opaque ID in future) |
| **Present** | `present_region()` / `swap()` — maps to Limine memcpy, `virtio_gpu_flush`, SVGA flush, AMD scanout, etc. |
| **Capabilities** | `FEATURE_SCANOUT`, `FEATURE_VIRGL`, `FEATURE_USERPTR`, … |

**Deliverable:** Refactor current Limine + virtio 2D + others behind this HAL **without** changing app-visible SDL APIs yet.

**Why it matters for Mesa:** The HAL is the **implementation** behind DRM ioctls; Mesa never calls the HAL directly. Keep HAL ops **GEM-like** (handles, map, unmap, submit) so mapping Linux ioctls → HAL stays mechanical.

---

## Phase B — Memory & handles (prep for APIs)

**Goal:** Stop passing raw `kalloc` pointers as the only “GPU memory.”

1. Introduce **buffer objects** (kernel handles + optional mmap from userspace later):
   - creation, destruction, map/unmap, size, alignment
2. For virtio-gpu: align with **resource IDs** and guest memory blobs you already use for 2D.
3. For bare-metal AMD: eventually **VRAM vs system** (gart/gtt) — document as future; start with **coherent system memory + command submission stub** if needed.

**Deliverable:** SDL can still use a CPU buffer, but that buffer is a **handle** through the HAL, not a direct `fb->address` cast everywhere.

---

## Phase C — Queues, fences, and submission (Vulkan precursor)

**Goal:** One place where “work” is submitted, even if the first “work” is a no-op or a 2D blit.

1. **Queue** abstraction: `submit(command_buffer)` or `submit(blit_list)` per device.
2. **Fence / timeline** (can be trivial spin-wait or IRQ-driven later).
3. virtio-gpu: path toward **context creation** and **VIRGL** (`VIRTIO_GPU_F_VIRGL` is already in your headers — treat as a **phase-C stretch** once 2D HAL is stable).

**Deliverable:** Internal API that could back `vkQueueSubmit` later without renaming everything.

---

## Phase D — Userspace vs kernel split (Mesa-first)

**Goal:** Match **Linux DRM + Mesa** so porting drivers is feasible.

| In kernel (inDevOS “DRM” KMD) | In userspace (Mesa) |
|------------------------------|---------------------|
| `/dev/dri/renderD*`, `card*` | `libdrm` + winsys |
| IOCTL: GEM, sync, driver CS | Command buffers, NIR, LLVM codegen |
| IRQ → fence signaling | Vulkan/OpenGL state trackers |
| Modeset on `card0` (optional) | Optional KMS-aware winsys / compositor |

**Deliverable:** Prefer documenting **DRM ioctl coverage** (spreadsheet: ioctl name → handler → HAL function) over ad-hoc `gpu_*` syscalls. If you expose extra syscalls, keep them **debug-only** so Mesa stays on the DRM path.

**FreeBSD note:** Mesa already runs on FreeBSD with a Linux DRM–compatible ioctl layer in the kernel; studying **drm-kmod** / FreeBSD’s DRM shim can inform inDevOS without requiring Linux itself.

---

## Phase E — API strategy: Vulkan first, OpenGL optional

**Recommended order**

1. **Vulkan 1.0 subset** (or even a **“inDevVK”** minimal subset): swapchain-like present, one graphics queue, buffers, images, one pipeline, draw calls without full feature parity.
2. **OpenGL** either:
   - **Much later**, via **Zink** (OpenGL on Vulkan) once your Vulkan is real enough, or  
   - A tiny fixed-function GL1-era subset only if you need compatibility for specific ports.

**virtio-gpu / QEMU:** Enabling **VIRGL** gives a well-trodden path: guest Vulkan/OpenGL via virglrenderer on the host — valuable for development before bare-metal AMD is fully wired.

**Bare-metal AMD:** Target **Mesa `vulkan_radeon` / radeonsi** by implementing the **AMDGPU DRM** subset that those winsys binaries expect (pin to one Linux LTS uAPI version). Your existing `ref/` AMD notes inform MMIO; Mesa supplies shader compilation.

---

## Phase F — SDL and desktop on top of the stack

1. **Short term:** SDL `RenderPresent` → HAL `present` (2D), unchanged semantics for apps.
2. **Medium term:** Optional **SDL_Renderer** backend that uses **Vulkan** for texture upload + quad draw (true GPU for 2D games).
3. **Desktop / compositor:** Eventually one **compositor process** using Vulkan to composite windows; kernel only exposes scanout and buffer sharing.

---

## Testing matrix (expand as drivers land)

| Target | 2D HAL | Vulkan / VIRGL | Notes |
|--------|--------|----------------|--------|
| Limine FB only | Required baseline | N/A | Regression tests |
| QEMU virtio-gpu (2D) | Yes | Later + VIRGL | Your current `virtio_gpu_flush` path |
| QEMU virtio-gpu + virgl | Yes | Stretch | Host needs virglrenderer |
| VMware SVGA | Yes | Limited / vendor-specific | Prioritize 2D parity |
| AMD bare metal | Yes | Long-term + Mesa-style | Use `ref/` hardware notes you already have |

---

## What to avoid

- **Don’t** embed OpenGL state inside the kernel.
- **Don’t** make SDL the only owner of framebuffer pointers — centralize in the HAL **and** expose them through **GEM mmap** when Mesa is in use.
- **Don’t** assume one global `fb`; multi-head and headless compute are easier if handles are per-device.
- **Don’t** invent parallel ioctl structs — **copy Linux layouts** for Mesa compatibility (version them explicitly).
- **Don’t** fork all of Mesa early — fork **winsys only** if you must; prefer completing DRM ioctls instead.

---

## Suggested next concrete steps (repo work)

1. Add **`docs/GPU_AND_GRAPHICS_API_ROADMAP.md`** (this file) — **done**.
2. Sketch **`gpu_hal.h`** with device ops — **done** (`src/gpu_hal.h`).
3. Migrate **Limine**, **virtio-gpu 2D**, **VMware SVGA**, **AMD GPU** to the HAL — **done** (`src/gpu_hal.c`).
4. Add a **capabilities log line** at boot — **done** (`gpu_hal_log_capabilities()`).
5. **Mesa track:** Add stub **DRM_IOCTL_VERSION** + **GET_CAP** — **done** (`src/drm.h`, `src/drm.c`).
6. **Mesa track:** Pinned **Linux 6.6 LTS uAPI**; coverage matrix — **done** (`docs/DRM_IOCTL_COVERAGE.md`).

### Next steps after Phase A

7. **Phase B:** Replace raw `fb->address` usage in `utils.c` `putpixel`/`fillrect`/`clear_screen` with HAL dispatch — unify all drawing through `gpu_hal_fill_rect` / `gpu_hal_clear`.
8. **Phase B:** Introduce **GEM-like buffer handles** in the HAL (create/destroy/map/unmap) so surfaces are never raw pointers at the API boundary.
9. **Phase C:** Add **queue/fence** abstraction to `gpu_device_ops` (`submit`, `wait_fence`); wire virtio-gpu control queue through it.
10. **Phase D:** Extend VFS with an `ioctl` op; register real `/dev/dri/card0` + `/dev/dri/renderD128` nodes backed by `drm_ioctl()`.
11. **Phase D:** Implement **GEM ioctls** (`CREATE_DUMB`, `MAP_DUMB`, `GEM_CLOSE`) backed by HAL buffer handles.
12. **Phase E:** Build and run **Lavapipe** (software Vulkan) as a first Mesa bring-up test.
13. **Phase E:** Enable **virtio-gpu VIRGL** path (requires virtgpu DRM ioctls from `docs/DRM_IOCTL_COVERAGE.md`).

---

## Phase A — Implemented types and entry points

### Core structs (`src/gpu_hal.h`)

| Struct | Purpose |
|--------|---------|
| `gpu_surface_t` | Buffer object: handle, pixels, width, height, pitch, bpp, format, size, driver_private |
| `gpu_device_ops_t` | Backend vtable: name, probe, init, shutdown, get_primary_surface, present_rect, present_all, fill_rect, clear, get_capabilities, get_width, get_height |
| `gpu_device_t` | Device instance: ops, caps, id, active, backend_data, primary_surface |

### Capability flags (`GPU_CAP_*`)

`SCANOUT` (0x01), `2D` (0x02), `3D` (0x04), `VIRGL` (0x08), `VULKAN` (0x10), `COMPUTE` (0x20), `USERPTR` (0x40), `DMA` (0x80)

### Registered backends (`src/gpu_hal.c`)

| Priority | Backend | Ops struct | Probe condition |
|----------|---------|-----------|----------------|
| 1 (best) | virtio-gpu | `virtio_ops` | `virtio_gpu_is_available()` |
| 2 | AMD GPU | `amd_ops` | `amd_gpu_is_available()` |
| 3 | VMware SVGA | `svga_ops` | `vmware_svga_is_available()` |
| 4 (fallback) | Limine FB | `limine_ops` | `fb != NULL` |

### DRM stub (`src/drm.h`, `src/drm.c`)

- **Pinned uAPI:** Linux 6.6 LTS
- **Minor numbers:** `DRM_MINOR_PRIMARY = 0` (`/dev/dri/card0`), `DRM_MINOR_RENDER = 128` (`/dev/dri/renderD128`)
- **Handled ioctls:** `DRM_IOCTL_VERSION` (returns `indevos` v0.1.0), `DRM_IOCTL_GET_CAP` (DUMB_BUFFER=1, depth=32)
- **Coverage matrix:** `docs/DRM_IOCTL_COVERAGE.md`

### Boot integration (`src/kernel.c`)

```
virtio_gpu_init()  →  amd_gpu_init()  →  vmware_svga_init()
                         ↓
                   gpu_hal_init()   ← probes all backends, selects best
                   drm_init()      ← self-tests VERSION + GET_CAP
```

---

## Revision

- **2026-03-23:** Initial roadmap aligned with multi-driver support and future OpenGL/Vulkan.
- **2026-03-23:** Mesa alignment: DRM ioctl UAPI, winsys/libdrm, bring-up order (Lavapipe → virtio → AMD), build notes, anti-patterns.
- **2026-03-23:** Phase A implemented: `gpu_hal.h/c`, `drm.h/c`, four backends (limine, virtio, AMD, SVGA), boot integration, DRM_IOCTL_COVERAGE.md.
