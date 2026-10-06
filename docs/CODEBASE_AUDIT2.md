# inDevOS Codebase Audit

**Date**: February 8, 2026
**Scope**: Full source tree under `src/` (~170 source files, ~45,000 lines of kernel C + assembly)
**Bootloader**: Limine (x86-64, higher-half kernel at `0xFFFFFFFF80000000`)
**Build system**: GNU Make + GCC cross-compiler + NASM
**Emulator target**: QEMU (q35 machine, various NIC/disk/USB configs)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture Overview](#2-architecture-overview)
3. [Subsystem-by-Subsystem Analysis](#3-subsystem-by-subsystem-analysis)
   - 3.1 [Boot & Kernel Core](#31-boot--kernel-core)
   - 3.2 [Memory Management (PMM + VMM)](#32-memory-management-pmm--vmm)
   - 3.3 [GDT, IDT & Interrupt Handling](#33-gdt-idt--interrupt-handling)
   - 3.4 [ACPI, APIC & Timers](#34-acpi-apic--timers)
   - 3.5 [PCI Bus Enumeration](#35-pci-bus-enumeration)
   - 3.6 [Scheduling & Threading](#36-scheduling--threading)
   - 3.7 [Process Management & Syscalls](#37-process-management--syscalls)
   - 3.8 [Filesystem & Storage](#38-filesystem--storage)
   - 3.9 [Networking Stack](#39-networking-stack)
   - 3.10 [Shell & Command System](#310-shell--command-system)
   - 3.11 [Desktop Environment](#311-desktop-environment)
   - 3.12 [Input Subsystem (Keyboard & Mouse)](#312-input-subsystem-keyboard--mouse)
   - 3.13 [Display & GPU Drivers](#313-display--gpu-drivers)
   - 3.14 [Lua Scripting Engine](#314-lua-scripting-engine)
   - 3.15 [Audio Subsystem](#315-audio-subsystem)
   - 3.16 [USB & xHCI](#316-usb--xhci)
   - 3.17 [SMP & Multi-core](#317-smp--multi-core)
   - 3.18 [Driver Management Framework](#318-driver-management-framework)
   - 3.19 [Kernel Modules (kmod)](#319-kernel-modules-kmod)
   - 3.20 [Utility & Support Code](#320-utility--support-code)
4. [Cross-cutting Concerns](#4-cross-cutting-concerns)
5. [What's Missing to Be "The Best OS"](#5-whats-missing-to-be-the-best-os)
6. [Prioritized Roadmap](#6-prioritized-roadmap)
7. [Final Verdict](#7-final-verdict)

---

## 1. Executive Summary

inDevOS is a hobby x86-64 operating system booting via the Limine protocol. It has an impressively **wide** feature surface for a hobby OS: physical + virtual memory management, a full TCP/IP networking stack, FAT32 read/write, a windowed desktop environment, Lua scripting, multiple NIC drivers, AC97 audio, SMP initialization, ELF loading, and a POSIX-inspired syscall interface.

### Strengths
- **Breadth of features**: Very few hobby OSes have a working TCP stack, desktop, Lua engine, AND multiple NIC drivers.
- **Well-structured networking**: Layered Ethernet/ARP/IP/TCP/UDP/DHCP/DNS with good separation of concerns.
- **Solid memory management**: Bitmap-based PMM with security validation and a complete 4-level VMM.
- **Good debugging**: Serial output, stack traces with symbol resolution, detailed page fault diagnostics.
- **Active development**: Large amount of code across many subsystems indicates sustained effort.

### Weaknesses
- **Everything runs in Ring 0**: No user/kernel separation — one bug can crash the whole system.
- **No true preemptive multitasking**: The preemptive scheduler in `idt.c` manipulates interrupt frames but the thread context save/restore is incomplete.
- **Many stubs**: NVMe, xHCI transfers, AMD GPU, VMware SVGA, file copy/move, `readdir`, several syscalls.
- **Concurrency gaps**: Most subsystems lack locking; the filesystem has no protection at all.
- **Two parallel command systems**: Both `cmd.c` and `zeus.c` exist with overlapping functionality.

### By the Numbers
| Metric | Value |
|--------|-------|
| Source files in `src/` | ~170 |
| Lines of C code | ~40,000+ |
| Lines of assembly | ~250 |
| Syscalls implemented | 33 (+ 15 SDL) |
| NIC drivers | 4 (RTL8139, E1000, PCnet, RTL810x) |
| Network protocols | 8 (Ethernet, ARP, IPv4, TCP, UDP, ICMP, DHCP, DNS) |
| Filesystems | 2 (FAT32 R/W, EXT2 read-only) |
| Storage drivers | 4 (AHCI, NVMe stub, IDE, ramdisk) |

---

## 2. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                    User Applications                          │
│  (ELF binaries, Lua scripts, built-in apps)                  │
├──────────────────────────────────────────────────────────────┤
│  Syscall Interface (int 0x80 + SYSCALL/SYSRET)               │
├──────────┬───────────┬────────────┬──────────────────────────┤
│ Zeus     │ Desktop   │ Lua        │ Command                  │
│ Shell    │ Manager   │ Engine     │ Processor                │
├──────────┴───────────┴────────────┴──────────────────────────┤
│  VFS Layer                                                    │
│  ├── FAT32 Driver (R/W)                                      │
│  └── EXT2 Driver (R/O)                                       │
├──────────────────────────────────────────────────────────────┤
│  Networking Stack                                             │
│  TCP ← UDP ← ICMP ← IP ← ARP ← Ethernet                    │
│  DHCP Client │ DNS Resolver                                   │
├──────────────────────────────────────────────────────────────┤
│  Driver Manager + Adapters                                    │
│  ├── NIC: RTL8139, E1000, PCnet, RTL810x                    │
│  ├── Storage: AHCI, NVMe, IDE, Ramdisk                      │
│  ├── Audio: AC97, PC Speaker                                 │
│  ├── GPU: VMware SVGA (stub), AMD (partial), VGA             │
│  ├── USB: Generic + xHCI                                     │
│  └── Input: PS/2 Keyboard, PS/2 Mouse                       │
├──────────────────────────────────────────────────────────────┤
│  Kernel Core                                                  │
│  ├── PMM (bitmap allocator) + VMM (4-level paging)           │
│  ├── GDT + TSS + IDT + APIC/IOAPIC                          │
│  ├── HPET Timer + LAPIC Timer                                │
│  ├── Thread Scheduler (cooperative + preemptive attempt)     │
│  ├── Process Scheduler (framework, not fully wired)          │
│  ├── ACPI + PCI enumeration (MCFG/MMIO)                     │
│  └── SMP Init (APs booted but idle)                          │
├──────────────────────────────────────────────────────────────┤
│  Hardware (x86-64 via QEMU q35)                              │
└──────────────────────────────────────────────────────────────┘
```

**Boot Flow**: Limine → `kmain()` → BSS clear → serial init → GDT → IDT → framebuffer/term → CPU/FPU → PMM → VMM → VGA → process scheduler → memory pools → ACPI → PCI → drivers → network → keyboard/mouse → storage → filesystem mount → Zeus shell → SMP → main loop

---

## 3. Subsystem-by-Subsystem Analysis

### 3.1 Boot & Kernel Core

**Files**: `kernel.c`, `linker.ld`, `limine.h`, `Makefile`

**What We Have**:
- Limine protocol bootloader with higher-half kernel mapped at `0xFFFFFFFF80000000`
- Proper linker script with separate text/rodata/data/bss segments
- Two-phase build: first build without symbols, then generate symbol table and relink
- BSS section manual clearing (since no C runtime)
- Comprehensive main loop with keyboard echo, tab completion, scrollback, USB polling, desktop rendering, network packet processing, and TCP cleanup
- Efficient CPU idling with `sti; hlt` when no work is pending

**Implementation Quality**: 7/10

**Issues**:
- `kernel.c` main loop is 200+ lines of inline logic — should be refactored into subsystem tick functions
- Tab completion logic is duplicated between keyboard and main loop
- `extern` declarations scattered throughout `kmain()` instead of proper headers
- `static uint64_t loop_count` is incremented but never used
- The `netset` command is hardcoded in `kmain()` — should be configurable

**Improvements Needed**:
- Extract main loop iteration into named subsystem functions
- Move hardcoded network config to a config file or boot parameter
- Add boot parameter parsing (from Limine or command line)

---

### 3.2 Memory Management (PMM + VMM)

**Files**: `mem.c`, `mem.h`, `bitmap.h`, `math.h`

**What We Have**:
- **PMM**: Bitmap-based physical page allocator using Limine memory map
  - `kalloc()` / `kfree()` with allocation cursor for O(n) allocation
  - `kalloc_high()` for DMA buffers above 1MB
  - Security: NULL checks, size overflow prevention, allocation size limits (max 25% of memory)
  - `PageEntry` map tracking allocation sizes for multi-page frees
  - Interrupt-safe via `irq_save()`/`irq_restore()` (no spinlocks to avoid deadlock)
- **VMM**: Full x86-64 4-level paging
  - Page table walking, creation, and manipulation
  - `vmm_map_page()`, `vmm_unmap_page()`, `vmm_is_mapped()`
  - Per-address-space operations via `vmm_map_page_in()`
  - Address space creation, cloning (for fork), switching, and destruction
  - TLB flush on map/unmap
- **Memory Pools**: Fixed-size block allocator for common allocation sizes (64B, 256B, 1KB)
- **Optimized `memcpy`/`memset`**: 64-bit word operations with loop unrolling for aligned memory

**Implementation Quality**: 8/10 — This is one of the strongest subsystems.

**Issues**:
- PMM uses a linear scan from cursor — fragmentation can make this slow over time
- No slab allocator for kernel objects (only the basic mempool)
- `kalloc()` returns zeroed memory only for `vmm_alloc_frame()` — general `kalloc()` returns uninitialized memory
- `memmove()` backwards copy doesn't use word-sized operations
- Memory pool `free_list` is stored at the end of the allocated pool memory — fragile layout
- No memory statistics export (used/free/fragmented) accessible from shell

**Missing**:
- Slab/SLUB allocator for kernel objects
- Demand paging / page fault handler for lazy allocation
- Copy-on-write (CoW) for fork
- Page cache for filesystem blocks
- Swap / memory pressure handling
- NUMA awareness

---

### 3.3 GDT, IDT & Interrupt Handling

**Files**: `gdt.c`, `gdt.h`, `gdtasm.asm`, `idt.c`, `idt.h`, `idtasm.asm`, `pic.c`, `pic.h`

**What We Have**:
- **GDT**: 7 entries — NULL, kernel CS/DS, user DS/CS (ordered for SYSCALL/SYSRET), TSS
  - TSS with RSP0 set for Ring 3 → Ring 0 transitions
  - 16KB kernel stack area
- **IDT**: 256 entries, vectors 0-31 (exceptions), 32 (LAPIC timer), 33 (PS/2 KB), 35 (HPET), 43-48 (device IRQs)
  - ISR stubs push all 15 general-purpose registers before calling C handler
  - Preemptive scheduling integrated into HPET timer handler
- **Exception Handling**: Excellent page fault diagnostics with:
  - Address analysis (NULL deref, kernel vs user space)
  - Error code breakdown (present/write/user/reserved/fetch)
  - Full register dump with RFLAGS breakdown
  - Stack dump with return address detection
  - Stack trace with symbol lookup
  - Lua context reporting
- **GPF Handler**: Similar detailed diagnostics with GDT/LDT selector analysis

**Implementation Quality**: 8/10

**Issues**:
- ISR stubs don't use IST (Interrupt Stack Table) — a double fault on a corrupted stack will triple-fault
- No ISR for vector 34 (gap between 33 and 35)
- The preemptive scheduler in `idt.c` only saves `rsp`/`rip` — GP registers are only partially restored
- `GDT_OFFSET_KERNEL_CODE` in `idt.h` is defined as `0x28` but actual kernel CS is at `0x08` — this constant is unused but misleading
- Page fault handler halts the CPU — no recovery mechanism for user-space faults

**Missing**:
- IST entries for double fault, NMI, and machine check
- Recoverable page fault handling (for demand paging, CoW, stack growth)
- Per-CPU IDT for SMP
- Nested interrupt support

---

### 3.4 ACPI, APIC & Timers

**Files**: `acpi.c`, `acpi.h`, `apic.c`, `apic.h`, `hpet.c`, `hpet.h`, `timer.c`, `timer.h`

**What We Have**:
- **ACPI**: RSDP → RSDT/XSDT parsing, finds MADT, FADT, HPET, MCFG tables
- **LAPIC**: Initialization, EOI, periodic timer calibration via PMT delay, IPI sending
- **IOAPIC**: Full redirection table management, GSI lookup, ISO (Interrupt Source Override) support
- **HPET**: High-precision event timer with one-shot and periodic modes
  - Frequency calculation from capabilities register
  - Counter-based sleep for precise delays
  - ISR with periodic comparator reset

**Implementation Quality**: 7/10

**Issues**:
- `calibrate_timer()` uses PMT delay of 5ms — may not be accurate on all hardware
- HPET ISR manually resets the comparator for periodic mode — this is a workaround for hardware that doesn't auto-reload (correct but fragile)
- LAPIC timer is calibrated but then disabled in `init_apic()` — only HPET is used
- Several `DEBUG_SERIAL()` calls have format strings but no variadic arguments (e.g., `"Added IOAPIC %d: ID=%d..."` with no args)
- `pmt_delay()` uses a busy-wait loop — acceptable for calibration but should be documented
- No ACPI power management (S-states, shutdown, reboot)

**Missing**:
- ACPI power management (shutdown, reboot, sleep states)
- ACPI AML interpreter (for device enumeration beyond basic tables)
- RTC (Real-Time Clock) integration for wall-clock time
- TSC (Time Stamp Counter) calibration for finer-grained timing

---

### 3.5 PCI Bus Enumeration

**Files**: `pci.c`, `pci.h`

**What We Have**:
- MCFG-based PCI Express memory-mapped configuration space access
- Full bus scan (256 buses × 32 devices × 8 functions)
- Multi-function device support
- Device list with class/subclass/vendor/device identification
- Read/write in 1/2/4 byte sizes

**Implementation Quality**: 7/10

**Issues**:
- Fixed array of 32 devices — will overflow on systems with more PCI devices
- No PCI capability list walking (MSI, MSI-X, PCIe)
- No BAR allocation/management
- Device string table is incomplete (many class/subclass combos return "Unknown")
- Memory leak if `check_function()` allocates but `vendor_id == 0xFFFF` on the re-read (line 200-201: allocates before checking vendor)

**Missing**:
- MSI/MSI-X support (currently relies on legacy IRQ routing)
- PCIe capability enumeration
- BAR resource management
- PCI bridge enumeration (recursive bus scan)
- Hot-plug support

---

### 3.6 Scheduling & Threading

**Files**: `thread.c`, `thread.h`, `threadasm.asm`, `simple_thread.c`, `simple_thread.h`

**What We Have**:
- **Thread Scheduler**: Create/destroy threads, ready/blocked/sleeping queues
  - Round-robin scheduling
  - Priority levels (HIGH, NORMAL, LOW, IDLE)
  - Spinlock-protected queue operations
  - `thread_yield()`, `thread_sleep()`, `thread_wake()`
- **Task Manager** (`simple_thread.c`): Cooperative timer-driven task system
  - Named tasks with configurable intervals
  - HPET-driven 1ms tick
  - Network maintenance task built-in
- **Preemptive Scheduling** (in `idt.c`): Timer-based context switch attempt
  - Saves current thread's RSP/RIP from interrupt frame
  - Restores next thread's RSP/RIP into interrupt frame for `iretq`
  - Wakes sleeping threads past deadline
  - 5-tick (5ms) time slice

**Implementation Quality**: 5/10 — The architecture is sound but the implementation is incomplete.

**Issues**:
- **Critical**: Preemptive scheduler only saves `rsp` and `rip` — all general-purpose registers are lost on context switch (they get zeroed for "fresh thread start")
- `thread_switch()` in `thread.c` creates a local `thread_context_t` on the stack and calls assembly — this will corrupt when the stack frame is destroyed
- `thread_switch_asm` and `thread_start_asm` are declared extern but their implementation quality is unknown (assembly not reviewed in detail)
- `scheduler_tick()` calls `thread_yield()` from interrupt context — this is dangerous as yield may sleep
- Two overlapping scheduling systems (thread scheduler + task manager) with unclear interaction
- The scheduler doesn't track time slices — priority levels exist but aren't used in scheduling decisions

**Missing**:
- Complete register save/restore on context switch
- Priority-based scheduling (not just round-robin)
- Thread local storage (TLS)
- Thread join/detach semantics
- Mutex/semaphore/condition variable primitives
- Proper idle thread
- CPU affinity for SMP

---

### 3.7 Process Management & Syscalls

**Files**: `syscall.c`, `syscall.h`, `syscallasm.asm`, `process_scheduler.c`, `process_scheduler.h`, `exec.c`, `exec.h`, `pipe.c`, `pipe.h`

**What We Have**:
- **Syscall Interface**: Dual-path — `int 0x80` (legacy) and `SYSCALL/SYSRET` (fast path)
  - STAR, LSTAR, SFMASK MSRs properly configured
  - 33 core syscalls + 15 SDL syscalls
  - Security validation on all syscall parameters
- **Process Table**: 16 process slots with PID tracking, CWD, file descriptors (32 max)
- **Implemented Syscalls**:
  - File I/O: `open`, `close`, `read`, `write`, `seek`, `stat`, `unlink`, `mkdir`
  - Memory: `brk`, `mmap` (simplified), `munmap`
  - Process: `fork`, `exec`, `exit`, `wait`, `getpid`
  - Signals: `signal`, `kill` (with handler dispatch and masking)
  - IPC: `pipe` (with circular buffer)
  - File descriptors: `dup`, `dup2`, `fcntl`, `ioctl`
  - Other: `sleep`, `time`, `print`, `getchar`, `getcwd`, `chdir`, `mount`, `unmount`
- **User Mode Entry**: `enter_usermode()` constructs iretq frame for Ring 3 transition
- **ELF Loader**: Loads ELF64 binaries with basic relocation support
- **Process Scheduler** (`process_scheduler.c`): Per-process PML4, kernel stack, thread association

**Implementation Quality**: 6/10

**Issues**:
- `sys_fork()` copies process struct but doesn't actually create a new schedulable entity — child never runs
- `sys_brk()` just updates a pointer without allocating/mapping memory
- `sys_mmap()` ignores addr/prot/flags and just calls `kalloc()` — no actual page mapping
- `sys_exec()` calls `exec_file()` which runs the binary in the current (kernel) context
- ELF loader's `fix_flat_binary_relocations()` uses heuristics — unreliable
- Signal handlers are called directly in kernel context (should push frame to user stack)
- `sys_readdir()` is a stub returning -1
- `sys_rmdir()` is a stub returning -1
- `copy_string_from_user()` does no proper validation — just tries direct access as fallback
- Process scheduler creates threads but doesn't wire CR3 switching or TSS RSP0 update

**Missing**:
- Working `fork()` + `exec()` with actual process creation
- Proper user-space memory management (page fault handling, CoW)
- `execve()` with argument and environment passing
- `waitpid()` with options
- Signal delivery via user stack (sigreturn trampoline)
- Process groups and sessions
- `select()`/`poll()`/`epoll()` for I/O multiplexing
- Proper `readdir()` implementation
- Socket syscalls

---

### 3.8 Filesystem & Storage

**Files**: `fat32.c/h`, `ext2.c/h`, `vfs.c/h`, `vfs_fat32.c/h`, `storage.c/h`, `ahci.c/h`, `nvme.c/h`, `ide.c/h`

**What We Have**:

#### FAT32 (Primary Filesystem)
- Full read/write support
- Long filename (LFN) support with checksum validation
- Directory create/delete, file create/delete
- Cluster allocation/deallocation via FAT table
- Path resolution with case-insensitive matching
- Format support

**Quality**: 7/10 — Most complete filesystem implementation

**Issues**:
- Only first FAT copy is updated (second copy diverges over time)
- No automatic directory expansion when full
- `fat32_seek()` doesn't free clusters when truncating
- No file locking for concurrent access
- No timestamp support (created/modified/accessed)

#### EXT2
- Read-only implementation
- Inode reading, file reading, directory listing
- Supports indirect, double-indirect, and triple-indirect blocks
- Multiple block sizes

**Quality**: 6/10 — Functional for reading but incomplete

#### VFS Layer
- Mount point management (max 8)
- File descriptor abstraction
- Function pointer dispatch to filesystem drivers

**Quality**: 5/10 — Good design, weak implementation

**Issues**: No locking, no reference counting, no symlink support, no path normalization

#### Storage Drivers
| Driver | Status | Quality |
|--------|--------|---------|
| AHCI | Functional (DMA, interrupt-driven) | 7/10 |
| IDE | Basic PIO mode | 5/10 |
| NVMe | **Stub only** (simulated reads) | 2/10 |
| Ramdisk | Working (16MB, FAT32 formatted) | 7/10 |

**Missing**:
- Write support for EXT2
- GPT partition table support (only MBR)
- Block device caching / page cache
- I/O scheduler
- TRIM/discard support
- Filesystem consistency checking (fsck)
- Journaling

---

### 3.9 Networking Stack

**Files**: `net_device.c/h`, `net_ethernet.c/h`, `net_arp.c/h`, `net_ip.c/h`, `net_tcp.c/h`, `net_udp.c/h`, `net_icmp.c/h`, `net_dhcp.c/h`, `net_dns.c/h`, `net_thread.c/h`, `rtl8139.c/h`, `e1000.c/h`, `pcnet.c/h`, `rtl810x.c/h`

**What We Have**:

| Layer | Protocol | Status | Quality |
|-------|----------|--------|---------|
| L2 | Ethernet | Complete | 7/10 |
| L2.5 | ARP | Complete (cache, retry, static entries) | 8/10 |
| L3 | IPv4 | Complete (fragmentation/reassembly) | 8/10 |
| L4 | TCP | Full state machine, retransmission, MSS | 7/10 |
| L4 | UDP | Complete with port handlers | 7/10 |
| L3 | ICMP | Basic (echo request/reply) | 5/10 |
| App | DHCP | Full client (DISCOVER→ACK, renewal) | 8/10 |
| App | DNS | A record resolution with caching | 6/10 |

#### NIC Drivers
| Driver | Status | Quality |
|--------|--------|---------|
| RTL8139 | Fully functional, interrupt-driven, DMA | 8/10 |
| E1000 | Basic init, descriptor rings, interrupts | 6/10 |
| PCnet | Basic init, ring buffers | 5/10 |
| RTL810x | Basic support | 5/10 |

**This is one of the strongest subsystems in inDevOS.** The TCP implementation has a proper state machine with 11 states, retransmission with exponential backoff, and flow control. The DHCP client is feature-complete.

**Issues**:
- TCP lacks congestion control (Tahoe/Reno/CUBIC)
- TCP options limited to MSS (no SACK, timestamps, window scaling)
- Only 16 simultaneous TCP connections
- DNS only resolves A records (no AAAA, MX, CNAME)
- ARP cache is only 16 entries
- IP busy-waits for ARP resolution (blocks the kernel)
- HPET tick ≠ millisecond assumption in several timeout calculations
- No statistics/counters exposed

**Missing**:
- IPv6
- Congestion control
- Socket API for user-space
- `select()`/`poll()` support
- Raw sockets
- VLAN support
- Network statistics
- Firewall/packet filtering
- TLS/SSL (even basic)
- HTTP client beyond demo

---

### 3.10 Shell & Command System

**Files**: `zeus.c`, `zeus.h`, `cmd.c`, `cmd.h`

**What We Have**:

#### Zeus Shell (Primary)
- Service-based command processing
- Built-in commands: filesystem ops, networking (ping, dhcp, dns, http, netset), process management, system info, thread tests, memory tests, desktop launch
- HTTP server demo
- Tab completion
- Ctrl+C interrupt support

#### Legacy Command Processor (`cmd.c`)
- Older command system with overlapping functionality
- File I/O commands, network commands, matrix animation
- Tab completion support

**Implementation Quality**: 6/10

**Issues**:
- Two command systems with overlapping functionality — confusing maintenance
- `exit` command doesn't actually halt/reboot
- `cd` doesn't update working directory in Zeus
- File copy/move are stubs
- `strtok()` used for parsing — non-reentrant
- Memory leaks in command argument parsing
- No command history
- No command piping (`|`)
- No I/O redirection (`>`, `<`, `>>`)
- No background processes (`&`)

**Missing**:
- Command history with arrow keys
- Piping and redirection
- Environment variables
- Shell scripting (if/else/for/while)
- Job control (fg/bg/jobs)
- Aliases
- Glob expansion

---

### 3.11 Desktop Environment

**Files**: `desktop_manager.c/h`, `desktop_renderer.c`, `desktop_apps.c`, `desktop_widgets.c/h`, `desktop_main.c`, `desktop_integration.c`

**What We Have**:
- Windows 11-style desktop with taskbar and window management
- Window operations: open, close, move, resize, minimize, maximize, focus
- Theme system with customizable colors
- Double-buffered rendering
- Alpha blending
- Built-in apps: Calculator, Text Editor, File Manager, Terminal, System Info
- Widget toolkit: Labels, Buttons, Checkboxes, Textboxes, Panels
- Event queue bridging kernel input to desktop

**Implementation Quality**: 6/10

**Issues**:
- Calculator buttons are non-functional
- File Manager uses dummy data (not connected to real filesystem)
- Terminal app has fragile scancode mapping
- Window iteration has off-by-one bug (skips last window)
- Maximize/restore doesn't save original window bounds
- Layer rendering system is mostly disabled
- `draw_rounded_rect()` doesn't actually draw rounded corners
- Cursor drawing code is verbose and duplicated
- `desktop_command_interface()` has hardcoded "start" string that prevents interactive use

**Missing**:
- Clipboard support
- Drag and drop
- Window snapping/tiling
- Virtual desktops
- Font scaling / TrueType fonts
- Hardware cursor
- Window decorations (close/minimize/maximize buttons that work)
- Right-click context menus
- Notification system
- File dialog
- Settings app

---

### 3.12 Input Subsystem (Keyboard & Mouse)

**Files**: `keyboard.c`, `keyboard.h`, `mouse.c`, `mouse.h`

**What We Have**:
- **Keyboard**: PS/2 interrupt-driven with scancode set 1 translation
  - Multiple layouts (US QWERTY, UK, German, French)
  - Modifier tracking (Shift, Ctrl, Alt, CapsLock)
  - Safe echo queue for deferred character output
  - Tab completion flag, PgUp/PgDn scrollback flags
  - Atomic character consumption for syscall use
- **Mouse**: PS/2 interrupt-driven with 3-button support
  - IntelliMouse extension detection
  - Coordinate tracking with screen bounds clamping
  - Deferred focus change processing

**Implementation Quality**: 7/10

**Issues**:
- Only left Ctrl is tracked (right Ctrl ignored)
- Backspace outputs space+backspace (visual artifact)
- Keyboard init has busy-wait loops for controller ACK
- Mouse screen dimensions can become stale after mode change
- No scroll wheel support (4-byte IntelliMouse protocol not fully handled)
- No key repeat rate configuration

**Missing**:
- Function key support (F1-F12)
- Key remapping / custom keymaps
- Mouse acceleration curve
- Scroll wheel
- Multi-touch support (for future touchpad)

---

### 3.13 Display & GPU Drivers

**Files**: `vga.c/h`, `vmware_svga.c/h`, `amd_gpu.c/h`, `term.c/h`

**What We Have**:
| Driver | Status | Notes |
|--------|--------|-------|
| Limine Framebuffer | Working (primary display) | Used by term.c for all output |
| VGA Text Mode | Working (80x25) | Used for matrix command |
| VMware SVGA-II | **Intentionally stubbed** | All functions are no-ops |
| AMD GPU | **Partial** | PCI detection, BAR mapping, placeholder registers |

- **Terminal** (`term.c`): Framebuffer console with custom `printf`/`vsnprintf`
  - Floating-point format support
  - Interrupt context tracking to prevent concurrent framebuffer access
  - Scrollback buffer

**Implementation Quality**: 5/10 (overall GPU support is weak)

**Issues**:
- No hardware-accelerated rendering — everything is software blitting to framebuffer
- VMware SVGA driver exists but is completely non-functional (intentional)
- AMD GPU writes to placeholder registers (not real GPU commands)
- No mode setting — stuck with whatever Limine provides
- Terminal has no ANSI escape sequence support

**Missing**:
- Virtio-GPU driver (most useful for QEMU)
- Mode setting (resolution/refresh rate changes)
- Hardware acceleration (2D blitting, fill)
- Hardware cursor
- Multiple displays
- VBE/VESA support as fallback
- ANSI terminal emulation

---

### 3.14 Lua Scripting Engine

**Files**: `luade.c/h`, `luade_graphics.c/h`, `luade_input.c/h`, `lua_runtime.c/h`, `libc_shim.c`, third_party/lua-5.4.7

**What We Have**:
- Lua 5.4.7 fully embedded in kernel
- Custom kernel allocator for Lua (interrupt-safe, SSE-enabled)
- Graphics API: `clear_screen`, `draw_pixel`, `draw_rect`, `draw_filled_rect`, `draw_line`, `draw_circle`, `present`
- Input API: `poll_event`, `wait_event` with keyboard/mouse event dispatch
- System API: `sleep`, `time`, `log`, `print`
- Double-buffered rendering with memory pool
- Script loading from FAT32 filesystem
- Instruction count limit hook for infinite loop protection

**Implementation Quality**: 7/10

**Issues**:
- `pcall()` protection for Lua errors is not fully wired — a Lua error can crash the kernel
- `draw_text()` in graphics API is a stub (no text rendering)
- `sleep()` uses busy-wait
- `get_key_state()` is a placeholder
- `realloc()` shim doesn't preserve existing data when growing
- `setjmp`/`longjmp` are stubs (`longjmp` calls `abort()`) — Lua error recovery broken
- No sandboxing — Lua scripts have full kernel access

**Missing**:
- Proper error recovery (working setjmp/longjmp)
- Text rendering in graphics API
- Image/sprite loading
- Sound API
- File I/O API for Lua
- Network API for Lua
- Script sandboxing (memory limits, API restrictions)
- Hot-reload for development
- Coroutine support (needs working setjmp/longjmp)

---

### 3.15 Audio Subsystem

**Files**: `ac97.c`, `ac97.h`, `sound.c`, `sound.h`

**What We Have**:
- **AC97**: PCI-based audio driver with DMA
  - Buffer Descriptor List (BDL) management
  - WAV file playback
  - Volume control
  - Polling-based buffer management
- **PC Speaker**: Simple beep/frequency generation via PIT

**Implementation Quality**: 4/10

**Issues**:
- AC97 uses polling instead of interrupts for buffer management
- WAV parser is very basic (no format validation)
- `PlayWAV()` in `sound.c` treats WAV data as PIT frequency divisors — fundamentally broken
- No audio mixing (can only play one stream)
- No recording support

**Missing**:
- Interrupt-driven audio playback
- Audio mixing (multiple streams)
- Recording
- HD Audio (Intel HDA) driver
- Audio format conversion
- Volume mixer
- MIDI support

---

### 3.16 USB & xHCI

**Files**: `usb.c`, `usb.h`, `xhci.c`, `xhci.h`

**What We Have**:
- **USB Framework**: Device registration, dummy keyboard device
  - Generic USB structure with endpoints and descriptors
  - Polling-based input integration
- **xHCI**: Host controller initialization
  - PCI device detection
  - MMIO register mapping
  - Device Context Base Address Array (DCBAA) setup
  - Command/Event/Transfer ring initialization
  - Port reset and device slot allocation

**Implementation Quality**: 4/10

**Issues**:
- Core USB operations (`control_transfer`, `bulk_transfer`, `interrupt_transfer`) are stubs
- Only a dummy keyboard device works (hardcoded, not from real USB enumeration)
- xHCI `control_transfer` is a placeholder
- No interrupt handler for xHCI
- No USB hub support
- No USB mass storage support

**Missing**:
- Complete USB enumeration (device descriptors, configuration, interface)
- USB mass storage class (for USB drives)
- USB HID class (proper keyboard/mouse from USB)
- EHCI/OHCI/UHCI support (for USB 2.0/1.1 controllers)
- USB hub support
- USB hot-plug
- USB power management

---

### 3.17 SMP & Multi-core

**Files**: `smp.c`, `smp.h`, `cpu.c`, `cpu.h`, `cpuid.c`, `cpuid.h`, `fpu.c`, `fpu.h`

**What We Have**:
- CPU detection via Limine SMP response
- BSP LAPIC ID tracking
- Per-CPU data structures
- AP (Application Processor) boot sequence via IPI
- AP initialization: GDT, IDT, LAPIC, FPU/SSE
- IPI mechanisms (all/specific CPU, with/without data)
- FPU/SSE initialization with self-test

**Implementation Quality**: 6/10

**Issues**:
- **APs just spin in an idle loop** — they're booted but never used for actual work
- No per-CPU scheduling (all threads run on BSP)
- `cpu_name()` allocates memory with `kalloc()` but never frees it
- No CPU topology detection (cores vs threads)
- No CPU frequency detection
- FPU state is not saved/restored per-thread

**Missing**:
- Per-CPU run queues and load balancing
- CPU affinity
- Per-thread FPU/SSE state save/restore (FXSAVE/FXRSTOR or XSAVE)
- Work stealing scheduler
- CPU hotplug
- Performance monitoring counters
- AVX/AVX-512 support

---

### 3.18 Driver Management Framework

**Files**: `driver_manager.c/h`, `driver_adapters.c/h`, `driver_commands.c/h`, `driver_file_format.h`

**What We Have**:
- Generic driver registration with lifecycle (init/deinit/enable/disable/reset)
- Device type classification (NIC, Storage, Display, Audio, Input, USB, Other)
- Auto-discovery from PCI device list
- Adapter layer bridging generic API to specific hardware drivers
- CLI interface for driver management (list, info, enable, disable, reset)
- Driver file format specification (header-only, not implemented)

**Implementation Quality**: 7/10 — Good architecture, incomplete implementation

**Issues**:
- Dynamic driver loading is designed but not implemented
- Many adapter `deinit`/`disable`/`reset` operations are stubs
- `load_time` field is always 0
- No driver dependency resolution
- No driver versioning

**Missing**:
- Dynamic driver loading from disk (ELF .ko files)
- Driver dependency management
- Driver signing/verification
- Power management hooks per driver
- Hotplug notification system
- Driver statistics/health monitoring

---

### 3.19 Kernel Modules (kmod)

**Files**: `kmod.c`, `kmod.h`

**What We Have**:
- ELF64 module loading from filesystem
- Symbol table parsing and resolution
- Section loading (.text, .data, .bss, .rodata)
- Relocation handling (R_X86_64_64, R_X86_64_PC32, R_X86_64_32, R_X86_64_PLT32)
- Module init/cleanup function calling
- Module listing

**Implementation Quality**: 7/10

**Issues**:
- Continues loading even when symbols are unresolved (should fail)
- No module unloading cleanup (memory not freed)
- No dependency tracking between modules
- No version checking
- Max 32 modules

**Missing**:
- Module dependency resolution
- Safe module unloading
- Module parameters
- Module versioning (modversions)
- Kernel symbol export table
- Module signing

---

### 3.20 Utility & Support Code

**Files**: `string.c/h`, `utils.c/h`, `serial.c/h`, `debug.c/h`, `registers.c/h`, `atomic.h`, `types.h`, `macros.h`, `bitmap.h`, `math.h`, `kernel_symtab.c/h`, `kernel_config.h`, `debug_config.h`

**What We Have**:
- **String library**: `strlen`, `strcmp`, `strcpy`, `strcat`, `strstr`, `strtok`, `atoi`, `itoa`, etc.
- **Atomic operations**: Compare-and-swap spinlocks, atomic get/set/inc/dec
- **Interrupt control**: `irq_save()`/`irq_restore()` with RFLAGS preservation
- **Serial debug**: COM1 output with `serial_printf()`
- **Debug system**: Allocation tracking, heap/stack checks, address analysis
- **Symbol table**: Auto-generated from `nm` output for stack trace resolution
- **Custom types**: All standard integer types redefined (not using `<stdint.h>`)

**Implementation Quality**: 7/10

**Issues**:
- `types.h` redefines `uint8_t` etc. instead of using compiler-provided `<stdint.h>` — could conflict
- `types.h` includes `term.h` creating a circular dependency risk
- `strtok()` uses a global static pointer — non-reentrant, unsafe in concurrent code
- Debug allocation tracker has a fixed 1024 entry limit
- No `snprintf()` — only `vsnprintf()` via `term.c`

---

## 4. Cross-cutting Concerns

### 4.1 Security
| Concern | Status |
|---------|--------|
| User/Kernel separation | **Not implemented** — everything Ring 0 |
| Syscall parameter validation | Good — bounds checks on all syscall args |
| Memory allocation limits | Good — max 25% of memory per allocation |
| Buffer overflow prevention | Partial — some bounds checks, many missing |
| Stack canaries | Not implemented |
| ASLR | Not implemented |
| NX bit | Defined in VMM but not enforced |
| Path traversal | Basic `..` check in `sys_open()` |

### 4.2 Concurrency & Locking
| Subsystem | Protection |
|-----------|-----------|
| PMM (kalloc/kfree) | `irq_save()`/`irq_restore()` — good |
| VMM | None — **dangerous** |
| FAT32 | None — **dangerous** |
| VFS | None |
| Thread scheduler | Spinlocks — good |
| Network device | None |
| PCI | None |
| Desktop | Deferred processing — partial |
| Serial output | None (safe due to single-threaded main loop) |

### 4.3 Error Handling
- **Kernel panics**: Used for critical init failures (RSDP, MADT, FADT, HPET, MCFG missing)
- **Exceptions**: Detailed diagnostics but always halt (no recovery)
- **Allocation failures**: Generally checked with NULL returns
- **Driver failures**: Inconsistent — some silently fail, some print errors

### 4.4 Code Quality
- **Documentation**: Good in headers (`mem.h` is excellent), sparse in implementation files
- **Naming conventions**: Mix of `snake_case` and `camelCase`; underscore prefix for internal functions
- **Dead code**: Some unused variables and commented-out code blocks
- **Warning suppression**: Many warnings disabled in Makefile (`-Wno-unused-*`, `-Wno-int-conversion`)
- **Build system**: Functional but has duplicate `ASM_SOURCES` definition

---

## 5. What's Missing to Be "The Best OS"

### Tier 1: Critical (Needed for a Functional General-Purpose OS)

1. **User/Kernel Separation (Ring 3)**
   - Currently everything runs in Ring 0. A single buffer overflow crashes the kernel.
   - Need: User-space page tables, syscall boundary, copy_to/from_user validation

2. **Working Preemptive Multitasking**
   - Current scheduler doesn't properly save/restore all registers
   - Need: Complete context switch, proper time slicing, priority scheduling

3. **Working fork() + exec()**
   - `fork()` creates a process entry but child never runs
   - `exec()` loads in kernel context without isolation
   - Need: Process creation with separate address space, ELF loading into user pages

4. **Demand Paging & Page Fault Recovery**
   - Page faults currently halt the CPU
   - Need: Lazy allocation, stack growth, copy-on-write, swap

5. **Filesystem Locking**
   - Zero concurrent access protection on FAT32/VFS
   - Need: Per-file locks, per-directory locks, or a big filesystem lock

6. **Complete Context Switch**
   - GP registers, FPU/SSE state, segment registers all need saving
   - Need: FXSAVE/FXRSTOR per thread, complete register save in assembly

### Tier 2: Important (Needed for Usability)

7. **Block Cache / Page Cache**
   - Every disk read goes directly to hardware
   - Need: LRU page cache for recently read blocks

8. **Working USB Stack**
   - USB enumeration, HID, mass storage — all stubs
   - Need: At least USB HID for keyboards/mice on modern hardware

9. **IPv6 Support**
   - Entirely missing — modern networks require it
   - Need: At minimum ICMPv6 + SLAAC

10. **Shell Improvements**
    - No piping, redirection, history, or scripting
    - Need: At least `|`, `>`, `>>`, `<`, and up-arrow history

11. **Real GPU Driver**
    - Virtio-GPU for QEMU, or at least working VMware SVGA
    - Need: Mode setting, hardware cursor, 2D acceleration

12. **EXT4 / Modern Filesystem**
    - FAT32 has no permissions, no journaling, 4GB file limit
    - Need: EXT4 or similar with journaling and large file support

13. **Proper Time/Date**
    - No RTC reading, no wall-clock time
    - Need: CMOS RTC driver, timezone support, `gettimeofday()`

14. **Network Socket API**
    - TCP/UDP work internally but aren't exposed to user-space
    - Need: BSD socket syscalls (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`)

### Tier 3: Nice to Have (For Competitive Feature Set)

15. **SMP Utilization**
    - APs are booted but idle
    - Need: Per-CPU run queues, load balancing, work stealing

16. **Dynamic Linking**
    - All binaries are statically linked
    - Need: Shared libraries, `ld.so` equivalent

17. **POSIX Signals (Proper)**
    - Current signal delivery is kernel-context only
    - Need: User-space signal frames, sigreturn trampoline

18. **Audio Mixing**
    - Can only play one audio stream
    - Need: Software mixer, multiple stream support

19. **TCP Congestion Control**
    - No congestion control at all
    - Need: At minimum TCP Reno or CUBIC

20. **Bootloader Configuration**
    - No boot parameters or boot menu
    - Need: Configurable boot options, kernel command line

21. **Package Manager / App Distribution**
    - No way to install software
    - Need: Simple package format, install/remove commands

22. **Networking Utilities**
    - No `wget`, `curl`, `ssh`, `telnet`
    - Need: At least a basic HTTP client and telnet

23. **Text Editor**
    - Desktop text editor exists but is basic
    - Need: Terminal-based editor (like nano) that works without desktop

24. **Permissions & Users**
    - No user accounts, no file permissions
    - Need: At minimum root/user distinction, file ownership

25. **Power Management**
    - No shutdown, reboot, or sleep
    - Need: ACPI S5 (shutdown), ACPI reset, S3 (sleep)

---

## 6. Prioritized Roadmap

### Phase 1: Stability (Weeks 1-4)
- [ ] Fix context switch to save/restore ALL registers (including FPU/SSE state)
- [ ] Add locking to FAT32 and VFS
- [ ] Fix page fault handler to recover (return to user-space) instead of halt
- [ ] Fix preemptive scheduler register save/restore
- [ ] Consolidate Zeus shell and cmd.c into one system
- [ ] Fix memory leaks identified in this audit
- [ ] Add IST entries for double fault and NMI

### Phase 2: User-Space (Weeks 5-10)
- [ ] Implement proper Ring 3 execution with user page tables
- [ ] Implement `copy_from_user()` / `copy_to_user()` validation
- [ ] Make `fork()` create a runnable child process with its own address space
- [ ] Make `exec()` load ELF into user pages with proper entry
- [ ] Implement demand paging (page fault → allocate → map → resume)
- [ ] Implement copy-on-write for fork
- [ ] Update TSS RSP0 on process switch

### Phase 3: I/O & Networking (Weeks 11-16)
- [ ] Implement block cache (LRU page cache for disk blocks)
- [ ] Implement BSD socket syscalls
- [ ] Add shell piping and redirection
- [ ] Add shell command history
- [ ] Implement RTC driver for wall-clock time
- [ ] Add `select()`/`poll()` syscalls
- [ ] Implement TCP congestion control (at least Reno)

### Phase 4: Hardware & Drivers (Weeks 17-22)
- [ ] Implement virtio-GPU driver for QEMU
- [ ] Complete USB HID stack (keyboard + mouse from USB)
- [ ] Implement MSI/MSI-X for PCI devices
- [ ] Put APs to work (per-CPU scheduling)
- [ ] Implement ACPI shutdown/reboot

### Phase 5: Polish (Weeks 23+)
- [ ] EXT4 filesystem support
- [ ] User accounts and file permissions
- [ ] Dynamic linking
- [ ] IPv6 (basic)
- [ ] Audio mixing
- [ ] Desktop improvements (clipboard, file dialogs, settings)
- [ ] Terminal ANSI escape sequences
- [ ] Package manager

---

## 7. Final Verdict

**inDevOS is an impressive hobby OS with extraordinary breadth.** Having a working TCP/IP stack, desktop environment, Lua engine, multiple NIC drivers, ELF loading, and FAT32 filesystem in a single kernel is a significant achievement. The codebase is generally readable and well-organized.

**The main gap is depth.** Many subsystems are started but not finished — the scheduler can't properly switch contexts, fork creates processes that never run, USB exists as stubs, and GPU drivers are placeholders. The biggest single issue is that everything runs in Ring 0 with no user/kernel boundary.

**To make this "the best OS", the priority should be:**
1. First, **stability**: Fix the context switch, add filesystem locking, handle page faults
2. Then, **user-space**: Ring 3 execution makes everything else possible
3. Then, **complete what exists**: Finish the syscalls, socket API, USB stack
4. Finally, **expand**: New filesystems, IPv6, SMP scheduling, GPU drivers

The foundation is solid. The architecture decisions (Limine, HHDM, 4-level paging, layered networking) are correct. With focused effort on depth over breadth, inDevOS can become a genuinely impressive operating system.

---

*Audit generated by comprehensive review of all ~170 source files in the `src/` directory.*
