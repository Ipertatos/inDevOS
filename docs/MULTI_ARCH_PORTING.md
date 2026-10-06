# Multi-arch rewrite and porting guide (`inDevOS-bak`)

This document is the **authoritative plan** for restructuring this tree into a maintainable, multi-architecture OS while **keeping Limine** as the boot protocol. It is based on a **source audit** of the current repository (May 2026), not only on intent.

**How to use it**

1. Read **§2 (audit)** — fix or accept blockers before large moves.
2. Follow **§4 (rewrite sequence)** — order matters; do not start with GUI or Lua.
3. Use **§5–6** as move maps when renaming paths.
4. Treat **§7** as the build contract for the new tree.

**Terminology**

| Label | Meaning |
|--------|---------|
| **Portable** | Same C compiles on every target; optional `#if ENABLE_LEGACY_PC` for PC-only drivers. |
| **Arch** | CPU/ISA: paging format, traps, syscall entry, context switch, I/O instructions. |
| **Platform** | Machine: ACPI+HPET PC vs `virt` UART vs RPi — often `kernel/platform/<name>/`. |
| **Host** | SDL “fake kernel” / dev runners — **never** linked into the Limine ELF. |

**References:** [Limine USAGE.md](https://github.com/limine-bootloader/limine/blob/trunk/USAGE.md), [OSDev — Limine](https://wiki.osdev.org/Limine).

---

## 1. Target repository layout

One **kernel ELF per architecture**. Top-level `ARCH=x86_64|aarch64|riscv64` selects `arch/<triple>/`, linker script, and Limine config.

```
inDevOS/                          # new or reorganized tree
├── Makefile                      # includes mk/arch/$(ARCH).mk, mk/kernel.mk
├── README.md
├── linker/
│   ├── x86_64/kernel.ld
│   ├── aarch64/kernel.ld         # when added
│   └── riscv64/kernel.ld
├── boot/
│   ├── limine-x86_64.cfg
│   ├── limine-aarch64.cfg        # later
│   └── README.md                 # QEMU cmdlines per arch + Limine version pin
├── configs/                      # optional: GDB, QEMU snippets
├── docs/
├── scripts/
│   ├── fetch_lua.sh
│   ├── symtab_gen.py
│   ├── build_iso_$(ARCH).sh
│   └── validate_boot_expect.sh
├── third_party/
│   ├── limine/                   # built artifacts + pinned headers (see §7)
│   ├── lua/                      # **one** version directory only
│   └── flanterm/                 # only if still used for early printk
├── include/
│   ├── kernel/                   # internal: types, debug_config, sys_internal
│   └── uapi/                     # syscall ABI: errno, stat, syscalls.h, kernel_api
├── kernel/
│   ├── main.c                    # thin: boot_info → subsystem init → idle loop
│   ├── boot/
│   │   └── limine_boot_info.c    # **only** place that touches Limine request structs
│   ├── mm/                       # physical allocator (portable) + high-level API
│   ├── sched/                    # thread + process policy
│   ├── syscall/
│   ├── fs/
│   ├── net/
│   ├── exec/
│   ├── ipc/
│   ├── tty/
│   ├── console/
│   ├── time/                     # ktime, generic timers
│   ├── scripting/                # Lua (optional)
│   ├── gui/                      # desktop, zeus, compositor (optional late)
│   └── debug/                    # kernel_symtab.c (generated)
├── arch/
│   ├── x86_64/
│   │   ├── asm/                  # syscall, thread switch, idt/gdt stubs
│   │   ├── cpu/                  # gdt, cpuid, msr, fpu
│   │   ├── intr/                 # idt, apic, pic, registers
│   │   ├── mm/                   # pmap, TLB, user copy with #PF handling
│   │   └── acpi/                 # or kernel/platform/pc/acpi
│   ├── aarch64/
│   └── riscv64/
├── hal/include/hal/              # barriers, io_read/write, panic decls
├── drivers/                      # pci, blk, net, usb, gpu, input, timer/
├── libk/                         # string, utils, bitmap helpers
├── userspace/
│   ├── libc/
│   ├── apps/                     # today: apps/, src/usoft/
│   └── ports/                    # doom/, herb/, demos
├── host-runner/                  # src/sdl_*.c — separate link target
└── build/                        # iso/, artefacts (gitignored)
```

**Design rules**

- **`kernel/`** = policy and portable algorithms.
- **`arch/`** = anything that mentions CR/MSR/APIC, NASM syntax, or x86 page tables.
- **`drivers/`** = devices; gate PC legacy with `CONFIG_LEGACY_PC` (or `ENABLE_LEGACY_PC`).
- **`hal/`** = thin types so `kernel/fs/vfs.c` never includes `cpuid.h` by accident.

---

## 2. Audit: current tree vs plan (verified)

### 2.1 What exists today

| Area | Location | Notes |
|------|----------|--------|
| Kernel C/asm | `src/*.c`, `src/*.h`, `src/*.asm`, `src/usoft/` | Flat; **no** `arch/` split |
| Limine header | `src/limine.h` only | Not under `third_party/`; must match built `limine/` binaries |
| Limine requests | Scattered | `mem.c` (memmap, HHDM), `utils.c` (framebuffer), `acpi.c` (RSDP), `cpu.c` (SMP) |
| Entry | `kmain` in `src/kernel.c` | ~640 lines of init + main loop |
| Linker | `linker.ld` | Higher-half `0xffffffff80000000`, `.limine_reqs` in `.data` |
| Build | `Makefile` | `find src`, NASM, `obj/` mirror, **SDL linked into kernel** |
| Public headers | `include/` | uAPI-style syscall headers |
| `kernel_api.h` | **Two files** | Root = 9-line stub; `include/kernel_api.h` = 370-line generated API |
| Lua | `third_party/lua-5.4` → symlink to `lua-5.4.7` | Makefile still adds **duplicate** `-I` and dead `C_SOURCES` wildcard |
| Limine boot | `limine.cfg`, `iso/` | Resolution split: interface 1280×800 vs entry 1920×1080 |
| Host SDL | `src/sdl_*.c`, `include/SDL.h` | **Present in `kernel_final.bin`** (symbols `SDL_Init`, etc.) |

### 2.2 Broken or incorrect (fix before or during phase 1)

**Status (pre-port fixes applied):** IOAPIC types, BSS init, uaccess PTE checks, `driver load`→kmod, ACPI PM timer fallback, signal handler policy, legacy USB fake enumeration, Makefile toolchain/Lua cleanup, `limine.cfg` resolution, `kernel_api.h` redirect, symtab dedupe, `-z noexecstack`. **Still deferred to port:** split in-kernel SDL shim to `host-runner/` (required for `SYS_SDL_*` today).

| Issue | Where | Impact |
|--------|--------|--------|
| **SDL in bare-metal link** | `Makefile` `boot.bin` / `kernel_final.bin` use `$(SDL_OBJ)` | ~2.5 MB kernel, fake SDL in ring 0, blocks clean multi-target builds |
| **`ioapic_configure_entry` type bug** | `src/apic.c` — passes `uint64_t*` to `ioapic_write(madt_ioapic_t*)` | Compiler warnings; wrong IOAPIC MMIO if layout diverges |
| **BSS clear fallback** | `src/kernel.c` — if `__bss_start == __bss_end`, clears `data_start+0x10000`..`data_end` | Can zero live `.data` on linker/layout changes |
| **`uaccess` is range-only** | `src/uaccess.h` — no page-table walk, no `#PF` recovery | `copy_*_user` can read/write unmapped user pages; security/stability |
| **Signal delivery** | `src/syscall.c` — Ring-3 trampoline “not implemented” | User signal handlers cannot run safely |
| **ACPI PM timer path** | `src/acpi.c` — `log_panic("PMTimer not implemented")` | Power/sleep paths may panic on some firmware |
| **Legacy USB control path** | `src/usb.c` — stubs unless xHCI | Non-xHCI controllers mostly non-functional |
| **`driver_load_from_disk`** | `src/driver_manager.c` — prints not implemented; still wired from `driver_commands.c` | Misleading operator UX |
| **Cross compiler detection** | `Makefile` `ifeq ($(wildcard ...), ...)` compares path to itself | Always false; never picks `~/opt/cross/bin/x86_64-elf-gcc` |
| **Duplicate `ASM_SOURCES`** | `Makefile` lines 63 and 66 | Harmless noise |
| **Executable stack note** | `obj/idtasm.o` missing `.note.GNU-stack` | Linker deprecation warning; harden NASM flags |
| **Limine header drift** | `src/limine.h` vs built `limine/` | Boot failures if header and boot media revisions differ |

### 2.3 Suboptimal (refactor during move)

| Issue | Recommendation |
|--------|----------------|
| **Monolithic `kernel.c`** | Split into `kernel/main.c` + `kernel/init/subsys_*.c` |
| **Limine requests in 4 files** | Centralize in `kernel/boot/limine_boot_info.c` → `struct boot_info` |
| **`mem.c` size** | Split: `kernel/mm/pmm.c`, `kernel/mm/vmm_api.c`, `arch/x86_64/mm/pmap.c` |
| **`-msse2` globally** vs `-mgeneral-regs-only` | SSE only on `libc_shim.o`, Lua objs, and explicit FP paths |
| **Desktop in `kmain` loop** | Comment in `kernel.c`/`desktop.c`: preemption does not safely preempt kernel stacks; compositor must stay on BSP loop until arch sched fixed |
| **All kernel threads in ring 0** | `threadasm.asm` — document; user tasks use separate `enter_usermode` path |
| **`find src` for all `.c`** | Exclude `src/usoft/` from kernel target; build as user apps |
| **Flanterm `-I` paths** | Makefile still passes `-Iflanterm` but comment says unused — remove or restore |
| **Symtab duplicates** | `kernel_symtab.c` lists some symbols twice — fix `symtab_gen.py` / nm filter |
| **Packed MADT pointers** | `apic.c` — use `memcpy` into locals instead of casting packed structs to `uint64_t*` |

### 2.4 What already works (preserve behaviour while moving)

- Limine handoff, HHDM, memmap-driven PMM in `mem.c`
- Higher-half link script with `.limine_reqs` retained in `.data`
- GDT/IDT/APIC bring-up, HPET + LAPIC timer
- Preemptive path for **spawned** Ring-3 processes (`exec_spawn`, `preemptive_schedule`)
- VFS + multiple FS backends, broad syscall surface in `syscall.c`
- SMP via Limine (`cpu.c` / `smp.c`)
- Two-phase link + `symtab_gen.py` for kernel symbols

---

## 3. Rewrite goals (definition of done)

**Phase A — x86_64 parity (same QEMU behaviour as today)**

- [ ] New directory layout; **one** `make ARCH=x86_64` produces `build/kernel-x86_64.elf` (or `kernel_final.bin` renamed).
- [ ] **No** SDL/host objects in the Limine kernel link line.
- [ ] `limine_boot_info.c` is the only Limine-touching compilation unit.
- [ ] ISO boots to desktop/shell with virtio-gpu + storage unchanged on `run-wsl` / `run`.
- [ ] Fixes in §2.2 for `ioapic_configure_entry`, Makefile toolchain probe, and BSS init (use linker symbols only; panic if BSS invalid).

**Phase B — clean platform boundary**

- [ ] PC drivers behind `CONFIG_LEGACY_PC`.
- [ ] `arch/x86_64/mm` implements `copy_from_user` with proper validation (range **and** user mappings).
- [ ] User signal trampoline or explicit “signals kill process” policy documented in uapi.

**Phase C — second architecture**

- [ ] `arch/aarch64` minimal: UART printk, exceptions, pmap, context switch.
- [ ] `boot/limine-aarch64.cfg` + `linker/aarch64/kernel.ld`.
- [ ] Same `struct boot_info` from Limine on AArch64.

---

## 4. Rewrite sequence (do in this order)

### Step 0 — Stabilize in place (still `src/`)

Do these **before** mass `git mv`:

1. **Split kernel link targets** in `Makefile`:
   - `kernel-$(ARCH).elf` ← all bare-metal objects.
   - `host-runner` ← `sdl_*.c` only (optional).
2. **Lua**: one include path (`third_party/lua-5.4.7` or rename to `third_party/lua`); remove `C_SOURCES += $(wildcard third_party/lua-5.4.7/...)` (it does not produce `obj/` rules anyway).
3. **Fix** `apic.c` `ioapic_configure_entry` to take `madt_ioapic_t *` (or `ioapic_redt_write` only).
4. **Fix** `kernel.c` BSS: require `__bss_start` / `__bss_end`; early `panic` if invalid — remove data-section heuristic.
5. **Pin** Limine: record `limine version` in `boot/README.md`; copy `limine.h` from the same revision into `third_party/limine/include/`.
6. **Reconcile** `kernel_api.h`: delete root stub or make it include `include/uapi/kernel_api.h` once.

### Step 1 — Skeleton directories (empty compiles)

Create `kernel/`, `arch/x86_64/`, `drivers/`, `libk/`, `include/kernel/`, `include/uapi/`, `hal/include/hal/`, `boot/`, `linker/x86_64/`, `host-runner/`.

- Move `linker.ld` → `linker/x86_64/kernel.ld` (update `LDFLAGS`).
- Move `limine.cfg` → `boot/limine-x86_64.cfg`; align `INTERFACE_RESOLUTION` and `RESOLUTION` with `Makefile` `VIRTIO_GPU_*`.

### Step 2 — Boot abstraction (highest leverage)

Add `kernel/boot/limine_boot_info.c` + `include/kernel/boot_info.h`:

```c
struct boot_info {
    uint64_t hhdm_offset;
    const struct limine_memmap_response *memmap;
    const struct limine_framebuffer *fb;  /* nullable */
    void *rsdp;
    const struct limine_smp_response *smp; /* nullable */
};
void boot_info_init(void);  /* fills from Limine; panics if mandatory missing */
const struct boot_info *boot_info_get(void);
```

- Move **volatile** `limine_*_request` globals from `mem.c`, `utils.c`, `acpi.c`, `cpu.c` into this file (or make them `static` there).
- Change `kmem_init` / `vmm_init` / `term_init` / `init_acpi` / SMP to read `boot_info_get()` only.

### Step 3 — `arch/x86_64` (link-critical)

Move in **link order** until `kernel-$(ARCH).elf` links after each batch:

| Batch | Files (from `src/`) |
|--------|---------------------|
| Asm | `*asm.asm`, `asmutils.asm` → `arch/x86_64/asm/` |
| CPU | `gdt.*`, `cpuid.*`, `msr.*`, `fpu.*`, `cpu.*` |
| Interrupts | `idt.*`, `apic.*`, `pic.*`, `registers.*` |
| MM arch | Page table walk, `vmm_map_*` PTE manipulation (extract from `mem.c`) |
| Syscall entry | `syscallasm.asm` + arch parts of `syscall.c` if any |

Keep **portable** PMM bitmap logic in `kernel/mm/` even if it still calls arch helpers for `invlpg`.

### Step 4 — `kernel/` policy

| From `src/` | To |
|-------------|-----|
| `kernel.c` | `kernel/main.c` (init calls only; move main loop body to `kernel/runloop.c` if needed) |
| `thread.c`, `process_scheduler.c`, `simple_thread.c` | `kernel/sched/` |
| `syscall.c` | `kernel/syscall/` (split arch dispatch table later) |
| `vfs*.c`, `fat32`, `ext2`, `ext4`, `pipe` | `kernel/fs/`, `kernel/ipc/` |
| `net_*` | `kernel/net/` |
| `exec.c`, `dynlink.c` | `kernel/exec/` |
| `timer.c`, `ktime.c` | `kernel/time/` |
| `debug.c`, generated symtab | `kernel/debug/` |
| `zeus`, `desktop`, `term`, `tty`, `tui`, `cmd`, `vconsole` | `kernel/gui/`, `kernel/console/` |

### Step 5 — `drivers/` and `libk/`

Move PCI, storage, net cards, USB, GPU, input, `hpet`, `rtc`, `serial`, `vga` per §6 table.

Move `string.c`, `utils.c` (portable subset) → `libk/`.

### Step 6 — Userspace and host

| From | To |
|------|-----|
| `apps/*.c` | `userspace/apps/` |
| `src/usoft/*` | `userspace/apps/` or `userspace/lib/` — **not** in kernel `find` |
| `doom/`, `herb_*`, `breakout.c`, etc. | `userspace/ports/`, `userspace/demos/` |
| `src/sdl_*.c` | `host-runner/` |

### Step 7 — Build artefacts

- ISO output → `build/iso-$(ARCH)/`.
- `symtab_gen.py` output → `kernel/debug/kernel_symtab.c`.
- Update `obj/` rules: `obj/kernel/%.o`, `obj/arch/x86_64/%.o`, etc.

### Step 8 — Only then: AArch64 / RISC-V

1. Copy skeleton `arch/aarch64/{asm,cpu,intr,mm}`.
2. Implement UART (PL011 on `virt`) + exception vector + identity/map kernel.
3. Add Limine cfg + linker script; extend `limine_boot_info.c` with `#if defined(ARCH_AARCH64)`.
4. Do **not** port PC drivers until core boots to `printk` + timer + scheduler smoke test.

---

## 5. File mapping: `src/` → new location

Legend: **P** portable, **A** x86-64 arch, **D** driver, **S** split, **H** host-only.

| Current (`src/`) | New path | Notes |
|------------------|----------|--------|
| `kernel.c` | `kernel/main.c` + optional `kernel/runloop.c` | **S** — shrink init |
| `limine.h` | `third_party/limine/include/limine.h` | Pin version |
| `gdt.*`, `gdtasm.asm` | `arch/x86_64/cpu/`, `arch/x86_64/asm/` | **A** |
| `idt.*`, `idtasm.asm` | `arch/x86_64/intr/` | **A** |
| `apic.*`, `pic.*` | `arch/x86_64/intr/` | **A** — fix IOAPIC types |
| `smp.*` | `arch/x86_64/` or `kernel/smp/` + arch IPI | **A** |
| `acpi.*` | `arch/x86_64/acpi/` or `kernel/platform/pc/` | PC-centric |
| `cpu.*`, `cpuid.*`, `msr.*`, `fpu.*` | `arch/x86_64/cpu/` | **S** |
| `registers.*` | `arch/x86_64/intr/` | **A** |
| `syscallasm.asm`, `threadasm.asm`, `asmutils.asm` | `arch/x86_64/asm/` | **A** |
| `syscall.*` | `kernel/syscall/` | **S** |
| `mem.*` | `kernel/mm/` + `arch/x86_64/mm/pmap.*` | **S** |
| `uaccess.*` | `kernel/mm/` + arch copy | **S** — strengthen |
| `atomic.h` | `hal/include/hal/atomic.h` | **S** |
| `string.*`, `utils.*` | `libk/` | **P** |
| `types.h`, `macros.h`, `bitmap.h`, `kernel_config.h`, `debug_config.h`, `sys_internal.h` | `include/kernel/` | **P** |
| `thread.*`, `process_scheduler.*`, `simple_thread.*` | `kernel/sched/` | **S** |
| `timer.*`, `ktime.*` | `kernel/time/` | **S** |
| `hpet.*`, `rtc.*` | `drivers/timer/` | **D** |
| `serial.*` | `drivers/uart/` or arch I/O | **D** |
| `vga.*` | `drivers/gpu/` | **D** |
| `exec.*`, `dynlink.*` | `kernel/exec/` | **S** |
| `vfs*`, `fat32`, `ext2`, `ext4`, `pipe` | `kernel/fs/`, `kernel/ipc/` | **P** |
| `pci.*`, `virtio*`, `ahci`, `nvme`, `ide`, `storage` | `drivers/` | **D** |
| `usb.*`, `xhci.*`, `keyboard`, `mouse` | `drivers/` | **D** |
| `net_*` | `kernel/net/` | **P** |
| `rtl8139`, `e1000`, `pcnet`, `rtl810x` | `drivers/net/` | **D** |
| `drm.*`, `gpu_hal.*`, `dev_dri_vfs.*`, GPU vendors | `drivers/gpu/` | **S** |
| `sound.*`, `ac97.*`, `audio_mixer.*` | `drivers/audio/` | **D** |
| `desktop.*`, `term.*`, `tty.*`, `zeus.*`, `tui.*`, `cmd.*`, `vconsole.*` | `kernel/gui/`, `kernel/console/` | **S** |
| `install.*`, `pkg.*`, `kmod.*` | `kernel/install/`, etc. | **P** |
| `lua_*`, `luade*` | `kernel/scripting/` | Optional |
| `driver_manager.*`, `driver_adapters.*`, `driver_commands.*` | `drivers/core/` | **P** |
| `sdl_*.c` | `host-runner/` | **H** |
| `usoft/*` | `userspace/` | User, not kernel |
| `profiler.*`, `debug.*`, `kernel_symtab.*` | `kernel/debug/` | **P** |

---

## 6. Files outside `src/`

| Current | New path |
|---------|----------|
| `linker.ld` | `linker/x86_64/kernel.ld` |
| `limine.cfg` | `boot/limine-x86_64.cfg` |
| `Makefile` | Root + `mk/arch/*.mk` |
| `symtab_gen.py` | `scripts/symtab_gen.py` |
| `include/*.h` | `include/uapi/` vs `include/kernel/` |
| `kernel_api.h` (root) | **Remove** — use `include/uapi/kernel_api.h` only |
| `apps/*.c` | `userspace/apps/` |
| `doom/`, demos, herb | `userspace/ports/`, `userspace/demos/` |
| `flanterm/` | `third_party/flanterm/` or drop |
| `third_party/lua-*` | `third_party/lua/` (single version) |
| `iso/`, `iso_full/` | `build/iso-*/` |

---

## 7. Build system contract

### 7.1 Flags

```make
# Example arch fragment (x86_64)
ARCH_CFLAGS  := -m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only
ARCH_CFLAGS  += -DARCH_X86_64 -DCONFIG_LEGACY_PC=1
KERNEL_INCLUDES := -Iinclude/kernel -Iinclude/uapi -Ithird_party/limine/include -Ihal/include
```

- **Do not** pass `-msse2` globally; only on files that need SSE (Lua, `libc_shim.c`, FPU init).
- **Do not** add `-Iflanterm` unless flanterm is actually compiled.

### 7.2 Objects

```make
KERNEL_C   := $(shell find kernel arch/$(ARCH) drivers libk -name '*.c')
KERNEL_ASM := $(wildcard arch/$(ARCH)/asm/*.asm)
# Exclude host-runner, userspace, tests
```

### 7.3 Limine ISO

- Kernel path in cfg: `boot:///boot/kernel-$(ARCH).elf` (or keep `boot.bin` name but document arch).
- Store **Limine commit/tag** beside cfg.
- `xorriso` + `limine bios-install` unchanged; only paths change.

### 7.4 Symbol table (keep two-phase link)

1. Link `kernel-nosym.elf` without `kernel_symtab.o`.
2. `nm -n` → `symtab_gen.py` → `kernel/debug/kernel_symtab.c`.
3. Final link `kernel.elf`.

Update generator paths when `kernel/` layout moves.

### 7.5 Assembler strategy

- **x86_64 today:** NASM is fine; add `section .note.GNU-stack noalloc noexec nowrite progbits` to asm files to drop executable-stack warning.
- **aarch64/riscv:** plan **GNU as** or clang integrated assembler; avoid maintaining two syntaxes long-term if possible.

---

## 8. Adding AArch64 or RISC-V

1. `linker/<arch>/kernel.ld` — follow Limine’s recommended load address for that arch.
2. `arch/<arch>/` — `early_uart.c`, `trap.c`, `pmap.c`, `context.S`.
3. `boot/limine-<arch>.cfg` — QEMU `virt` machine cmdline in `boot/README.md`.
4. Extend `boot_info_init()` — `#if ARCH_AARCH64` for framebuffer, UART, memmap (fields differ; never assume RSDP exists).
5. Reuse `kernel/fs`, `kernel/net` only after syscall + copy_to/from_user work on that arch.

---

## 9. Quick checklist (pre-flight)

- [ ] SDL **not** in `kernel` link
- [ ] Single Lua tree and single `-Ithird_party/lua/...`
- [ ] One `kernel_api.h` (uapi)
- [ ] Limine header revision = boot binary revision
- [ ] `limine_boot_info.c` owns all requests
- [ ] `ioapic_configure_entry` type-correct
- [ ] BSS init does not use data-section fallback
- [ ] `usoft/` and `apps/` built as user programs
- [ ] Resolution consistent: `limine.cfg` ↔ `VIRTIO_GPU_X/Y`
- [ ] Generated symtab path updated in Makefile + script

---

## 10. Further reading

- [Limine USAGE.md](https://github.com/limine-bootloader/limine/blob/trunk/USAGE.md)
- [OSDev — Porting](https://wiki.osdev.org/Porting)
- [xv6 book](https://pdos.csail.mit.edu/6.828/) — clean arch/kernel split
- [OSTEP](https://pages.cs.wisc.edu/~remzi/OSTEP/) — VM and scheduling theory

---

*Last updated from repository audit: flat `src/`, Limine boot, x86-64-centric drivers, SDL linked into production kernel, no `arch/` directory yet.*
