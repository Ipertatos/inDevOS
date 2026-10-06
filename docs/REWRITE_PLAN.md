# inDevOS Rewrite Plan

**Status:** Draft — based on the July 2026 full-codebase review (core kernel, storage/FS, networking, graphics/GUI, drivers/misc).

This document is the blueprint for rewriting inDevOS from scratch. It explains **why** a rewrite is justified, **what** to keep from the current codebase, the **contracts** the new kernel must establish before any subsystem is written, and a **phased plan** with concrete deliverables and exit criteria per phase.

---

## Table of contents

1. [Why rewrite instead of refactor](#1-why-rewrite-instead-of-refactor)
2. [Scope decisions (make these first)](#2-scope-decisions-make-these-first)
3. [What to keep, port, and delete](#3-what-to-keep-port-and-delete)
4. [Cross-cutting contracts (Phase 0)](#4-phase-0--cross-cutting-contracts-and-scaffolding)
5. [Phase 1 — Memory and CPU foundations](#5-phase-1--memory-and-cpu-foundations)
6. [Phase 2 — Scheduling, processes, and blocking primitives](#6-phase-2--scheduling-processes-and-blocking-primitives)
7. [Phase 3 — Storage stack](#7-phase-3--storage-stack)
8. [Phase 4 — Networking stack](#8-phase-4--networking-stack)
9. [Phase 5 — Input and USB](#9-phase-5--input-and-usb)
10. [Phase 6 — Graphics and desktop](#10-phase-6--graphics-and-desktop)
11. [Phase 7 — Userland, shell, and modules](#11-phase-7--userland-shell-and-modules)
12. [Testing strategy](#12-testing-strategy)
13. [Appendix A — Full stub/fake inventory of the old codebase](#appendix-a--stubfake-inventory-of-the-old-codebase)
14. [Appendix B — Worst bugs of the old codebase (root-cause classes)](#appendix-b--worst-bug-classes-in-the-old-codebase)
15. [Appendix C — Performance anti-patterns to avoid](#appendix-c--performance-anti-patterns-in-the-old-codebase)

---

## 1. Why rewrite instead of refactor

The review found that the problems are not localized bugs — they are **missing foundations** that every subsystem worked around independently:

| Missing foundation | How every subsystem compensated |
|---|---|
| No blocking/wait-queue primitive | Pipes return 0 instead of blocking; `sys_wait` returns `-ECHILD` immediately; TCP connect returns before ESTABLISHED; ARP/DNS/DHCP busy-wait and re-enter the RX path; keyboard/mouse are polled from the main loop |
| No per-CPU state | Single global `current_process`, single `bsp_gs` for syscall stacks, scheduler explicitly disabled on APs — SMP is bring-up theater |
| No lock-ordering or lock-type policy | TCP holds a non-recursive spinlock across sends (deadlock); VFS/FAT32 hold global locks across disk I/O; half the driver-manager and desktop getters read shared lists unlocked |
| No single time API | Milliseconds re-divided by HPET frequency in ARP and TCP cleanup → timers that never fire; SDL timer hardcodes 14.318 MHz |
| No buffer-ownership convention | Desktop frees window pixels the SDL client still holds (UAF); TCP callbacks receive pointers into stack RX buffers; drivers/stack/protocols each memcpy defensively |
| No fail-closed policy | kmod/dynlink load with unresolved symbols left as 0; `desktop_create_window` succeeds with NULL pixel buffer; NVMe returns success with garbage data |

Retrofitting these into 85k lines file-by-file costs more than rewriting on correct foundations and porting the good parts. The phases below are ordered so that **each phase only depends on contracts already established**, never on "we'll fix that later."

---

## 2. Scope decisions (make these first)

Every stub in the old tree exists because scope was implicit. The new tree declares scope explicitly and **deletes what is out of scope rather than stubbing it**.

### In scope (v1)

| Area | Decision |
|---|---|
| Architecture | x86-64 only, higher-half, Limine boot protocol |
| Test targets | QEMU first-class (virtio-gpu/virtio-vga, virtio-net, AHCI, NVMe). Real hardware is best-effort after QEMU is solid |
| CPU | SMP from day one at the *design* level (per-CPU everything), even if only 1 CPU is scheduled until Phase 2 completes |
| Storage | AHCI + NVMe (real) + RAM disk. FAT32 read/write, ext2 read/write |
| Network | virtio-net + E1000. IPv4, ARP, ICMP, UDP, TCP, DHCP client, DNS client |
| Graphics | Limine framebuffer + virtio-gpu. Kernel compositor, in-kernel SDL shim for ported apps |
| Userland | Ring-3 static ELF (PIE), Linux-like errno ABI, blocking syscalls, real coreutils, Zeus shell in userspace |
| Modules | Deferred to Phase 7; monolithic kernel until then |

### Explicitly out of scope (v1) — delete, don't stub

- VMware SVGA-II, AMD GPU / AtomBIOS, RTL810x, PCnet (keep source in an `attic/` reference dir if desired, never linked).
- IPv6 (the old one demuxes v6 frames it cannot deliver — worse than absent).
- ext4 write support (old extent-write path corrupts data; ext4 read-only *may* come post-v1).
- DRM/`/dev/dri` (old one advertises capabilities it doesn't have; reintroduce only with honest dumb-buffer support).
- USB mass storage, SDL audio, in-kernel Lua (LuaDE) — revisit post-v1.
- Windows-native build scripts (maintenance burden; WSL/Linux build only).

---

## 3. What to keep, port, and delete

### Port with cleanup (genuinely good pieces)

| Component | Old location | Notes |
|---|---|---|
| Two-phase link for kernel symbol table | `Makefile`, `symtab_gen.py`, `kernel_symtab.c` | Best-in-class part of the old build; keep for backtraces |
| Per-object SSE policy (`-mgeneral-regs-only` global, `-msse2` per object) | `Makefile` | Correct pattern, keep |
| AHCI driver | `ahci.c` | The most complete real driver: batched DMA, interrupt-driven with poll fallback. Port onto the new block layer |
| DHCP client logic | `net_dhcp.c` | Structured, bounds-checked option parsing. Port the protocol logic; replace the busy-wait pump with a blocking task |
| Virtio transport + virtio-gpu protocol code | `virtio.c`, `virtio_gpu.c` | Real device protocol; replace busy-poll completion with IRQs |
| Compositor concepts | `desktop.c` | Dirty rectangles, damage-union present, per-window fast path, dock/panel UX — keep the *design*, rewrite the code with a lifetime contract |
| Zeus shell UX | `zeus.c` | History, pipes, redirection, builtins — port to userspace on the real syscall ABI |
| uaccess helpers | `uaccess.c` | Range + PTE user-bit checks, byte-safe `strncpy_from_user` — good base, apply *uniformly* |
| Limine boot path, GDT/IDT/TSS bring-up | `kernel.c`, `gdt.c`, `idt.c` | Boot sequence is fine; interrupt *policy* changes (see Phase 1) |
| E1000 / RTL8139 register knowledge | `e1000.c`, `rtl8139.c` | Port register programming; delete hardcoded IPs/MACs/IRQ numbers |

### Rewrite from scratch (design is wrong, not just buggy)

PMM/heap, VMM COW paths, scheduler + thread lifetime, process model (fork/wait/exec/signals), pipes, VFS + FAT32 + ext2, block layer, entire TCP/socket layer, ARP, USB/xHCI, PS/2 input, PNG decoding (replace with vetted library), driver manager, kmod loader.

### Delete outright

Everything in the "out of scope" list above, plus: `nvme.c` (fake — write a real one), legacy `usb.c` control path, `timer.c` (dead PIT code), `sound.c` PC-speaker WAV player, kernel-linked `apps/*.c` stubs (`ls.c`, `cat.c`, `echo.c`, … — they include kernel headers and never built as user programs), `file_dialog.c`, duplicate driver registration tables, the flat-binary "heuristic relocation fixer" in `exec.c` (it patches immediates by guesswork and can corrupt valid code).

---

## 4. Phase 0 — Cross-cutting contracts and scaffolding

**Duration:** 1–2 weeks. **Nothing else starts until this is written down.**

### 4.1 Repository layout

```
kernel/
  arch/x86_64/        # boot, gdt/idt, apic, smp trampolines, context switch asm
  mm/                 # pmm, vmm, slab, uaccess
  sched/              # threads, scheduler, wait queues, per-cpu
  ipc/                # pipes, futex-like primitives (later)
  fs/                 # vfs, fat32, ext2, devfs
  block/              # block layer, bcache, partition parsing
  net/                # core, ethernet, arp, ipv4, udp, tcp, dhcp, dns, sockets
  drivers/
    block/ahci, block/nvme, net/virtio_net, net/e1000,
    gpu/virtio_gpu, input/ps2, input/xhci, bus/pci, acpi/
  video/              # compositor, console
  lib/                # string, printf, rb-tree/list, vetted PNG decoder
  include/            # kernel headers, syscall ABI shared with userland
user/
  libc-min/           # syscall wrappers, crt0, minimal libc
  bin/                # zeus, ls, cat, sh, ...
tools/                # symtab_gen.py, image scripts
tests/                # unit + QEMU integration tests
attic/                # old-tree reference code, never built
```

### 4.2 Time API (kills an entire bug class)

One header, one implementation, used by *everything*:

```c
uint64_t time_ns(void);     // monotonic, HPET or TSC-calibrated
uint64_t time_ms(void);     // convenience: time_ns() / 1000000
void     sleep_ms(uint64_t) // BLOCKS the calling thread (Phase 2+); panics in IRQ context
```

Rules: **no raw `hpet_get_ticks()` or frequency math anywhere outside `ktime.c`**. All timeouts stored as `deadline_ns`. (Old-tree consequence of violating this: ARP entries never expired, TCP TIME_WAIT never fired, SDL frame pacing wrong.)

### 4.3 Locking policy

- Two lock types only: `spinlock_t` (IRQ-save variant included; never held across anything that can sleep or perform I/O) and `mutex_t` (sleepable, Phase 2; the default for filesystem/socket state).
- **Written lock-ordering document** (`docs/locking.md`) updated whenever a lock is added. Order violations are review-blockers.
- Hard rule from the TCP deadlock: **never call a transmit/IO function while holding a state spinlock.** Prepare under the lock, send outside it.
- Every shared list/table has a named lock. "Reader is probably fine" (old `desktop_get_focused`, `driver_find_by_id`) is not accepted.

### 4.4 Buffer ownership convention

- Every allocation that crosses a subsystem boundary documents its owner in the function contract.
- Network: single `netbuf` (skbuff-like) type with refcount; drivers allocate, stack consumes, **no pointers into caller stack buffers ever escape a function**.
- Graphics: window buffers are owned by the *client* (process); the compositor holds a reference; freeing happens only on client release or process teardown. (Fixes the close-button UAF class.)

### 4.5 Fail-closed policy

- `KASSERT(cond)` panics in debug builds; `KWARN_ONCE` for recoverable oddities.
- Allocation failure, unresolved symbol, unsupported feature ⇒ **return an error to the caller.** No "continue anyway", no "return success and log", no silently rewriting invalid input (old PNG decoder rewrote invalid color types instead of rejecting).
- Syscalls: every user pointer goes through `copy_from_user`/`copy_to_user` — no exceptions, enforced by review checklist.

### 4.6 Build system

- Keep: two-phase symbol-table link, dependency files (`-MMD -MP`), per-object SSE overrides, `-mcmodel=kernel -mno-red-zone -fno-omit-frame-pointer`.
- Add: per-directory sources instead of a single `find src`, `make test` (unit tests compiled for host), `make qemu-test` (scripted integration boots), warnings promoted — remove `-Wno-unused-variable`/`-Wno-sign-compare` suppressions from day one.

**Exit criteria:** repo skeleton builds an empty kernel that boots in QEMU to a serial "hello"; `docs/locking.md`, `docs/ownership.md`, `docs/time.md` exist; CI (or a script) runs unit tests + a QEMU smoke boot.

---

## 5. Phase 1 — Memory and CPU foundations

**Duration:** 3–5 weeks.

### 5.1 Physical memory manager

- Buddy allocator (or freelist-per-order) over the Limine memory map. Replaces the old bitmap + first-fit cursor scan, which was O(pages) per `kalloc` with IRQs disabled.
- Page frame database: one `struct page` per frame with a **32-bit atomic refcount** (old tree used `uint8_t`, saturating at 255 into permanent leaks) and flags.
- All refcount changes through `page_ref()/page_unref()` atomics — the old tree's page-fault handler decremented refcounts non-atomically, bypassing its own CAS helpers.

### 5.2 Kernel heap

- Slab allocator on top of the buddy PMM for small objects (thread structs, netbufs, vnodes). The old tree page-granularity-allocated everything, wasting memory and fragmenting.
- `kmalloc/kfree` with size classes; debug build adds redzones + allocation tracking (the old `debug_track_allocation` existed but was never wired in).

### 5.3 Virtual memory

- Per-process address space object owning a PML4; upper half shared with kernel.
- Demand paging + COW as in the old tree, but: COW faults use atomic page refcounts only, and **TLB shootdown IPIs from day one** on any PTE downgrade/unmap (the old tree defined IPI vector 254 and never used it).
- `mmap/munmap/brk` operate only on user address spaces — the old `sys_mmap` returned raw kernel `kalloc` pointers for non-usermode callers, breaking the memory model.
- Guard page below every kernel stack.

### 5.4 Per-CPU infrastructure (the SMP fix)

Even while only the BSP schedules, build it right:

- `struct percpu { cpu_id, lapic_id, current_thread, current_process, kernel_stack_top, syscall_scratch, runqueue, ... }` accessed via `GS`.
- Fix the old `cpu.c` bug class at the source: **APs are identified by `cpus[i]->lapic_id`, never by array index** (the old code compared index to BSP LAPIC ID — worked on QEMU by accident).
- Each AP: sets up its own GS, GDT/TSS, IDT load, LAPIC timer calibration, then parks in a proper idle thread (not a raw `hlt` loop outside the scheduler).
- SYSCALL/SYSRET MSRs per CPU; per-CPU kernel stacks in the TSS (old tree had a single `bsp_gs` shared by all CPUs).

### 5.5 Interrupts

- IDT installed early, but **`sti` happens exactly once**, at a single well-defined point after all early init (the old `initIDT` did `sti` mid-boot, opening a window where timer/net ISRs ran against half-initialized subsystems).
- ISRs do minimal work and defer to threads (Phase 2 workqueues). No HPET reads, no heavy parsing, no serial printf storms in IRQ context.

**Exit criteria:** boots on QEMU with `-smp 4`; all APs online with working per-CPU data and LAPIC timers; stress test: parallel kalloc/kfree torture on all CPUs with refcount verification; COW fork-style page sharing unit-tested via a kernel self-test.

---

## 6. Phase 2 — Scheduling, processes, and blocking primitives

**Duration:** 3–5 weeks. This phase removes the root cause of most old-tree fakery.

### 6.1 Threads and scheduler

- Kernel threads and user threads are the same object with different entry setup. **All threads are preemptible**, including kernel threads — per-thread kernel stacks, context switch in the timer ISR path. (The old tree could only preempt user threads; the desktop had to run inside the kernel main loop because kernel stacks couldn't be switched.)
- Per-CPU runqueues with O(1) pick (bitmap of non-empty priority levels). Idle thread per CPU. Load balancing can be dumb (steal on idle) — correctness first.
- Thread exit fully reclaims resources: the exiting thread marks itself dead and the scheduler/reaper frees stack + TCB after the final switch. (The old `thread_exit` parked in `hlt` forever and leaked TCB, stack, and context nodes.)

### 6.2 Wait queues — the keystone primitive

```c
void wait_on(waitqueue_t *wq, mutex_t *held);   // atomically release + sleep
void wake_one(waitqueue_t *wq);
void wake_all(waitqueue_t *wq);
int  wait_timeout(waitqueue_t *wq, mutex_t *held, uint64_t deadline_ns);
```

Consumers from day one: `sleep_ms`, pipe read/write, `sys_wait`, keyboard/input reads, TCP connect/accept/recv, DHCP/DNS waits. **Any code that would busy-wait must use these instead** — busy-waits outside arch bring-up are review-blockers.

### 6.3 Process model

- `struct process`: address space, fd table, children list, exit status, signal state. Slab-allocated — no fixed `MAX_PROCESSES` array with two competing allocators keyed on different fields (the old `allocate_process` vs `process_alloc` disagreed on what "free slot" meant).
- **`fork`**: real COW fork. Parent continues running; child gets a copied thread context. No vfork-style parent suspension via setjmp/longjmp (the old implementation shared one kernel thread between parent and child).
- **`wait`/`waitpid`**: blocks on the child wait queue; reaps zombies; `WNOHANG` supported. Never returns `-ECHILD` just because no child has exited *yet*.
- **`exec`**: static PIE ELF loader; builds argc/argv/envp on the user stack per SysV ABI (the old one hardcoded `argc=1` and ignored `argv` entirely); handles `R_X86_64_RELATIVE/64/GLOB_DAT/JUMP_SLOT`. No flat-binary heuristics.
- **Signals**: implemented honestly or minimally. v1 target: default dispositions + `SIG_IGN` + user handlers with a kernel-built signal frame and a `sigreturn` trampoline mapped in user space. If the trampoline slips, `sigaction` **rejects** custom handlers instead of silently storing dead pointers.

### 6.4 Syscall layer

- One dispatch table (array of function pointers, not a 3000-line switch).
- Every entry validated: numbered args, `copy_*_user` for all pointers, `-ENOSYS` only for numbers genuinely outside the table.
- Pipes: blocking ring buffer on wait queues; `read` on empty pipe with writers blocks; returns 0 only at true EOF; `EPIPE`/`SIGPIPE` on writing to a closed pipe.

**Exit criteria:** stress: 100 processes fork/exec/wait in a loop for minutes without leaks (page counts return to baseline); pipeline `cat file | grep x | wc -l` works with real blocking; kernel threads preempt each other under load on all CPUs; signal delivery round-trips through a user handler and `sigreturn`.

---

## 7. Phase 3 — Storage stack

**Duration:** 3–4 weeks.

### 7.1 Block layer

- `struct blockdev` with an ops vtable per device instance — no dispatch-by-PCI-class with mismatched `extern` prototypes (the old NVMe/`storage.c` signature mismatch passed a `storage_device_t*` where a `uint8_t` was expected: undefined behavior before the fake body even ran).
- Request-based API (`bdev_read/write(dev, lba, count, buf)`) with a per-device mutex or queue; interrupt-driven completion where drivers support it.
- Unified buffer cache: hash-indexed, LRU, write-back with explicit `bdev_flush()` ordering hooks for filesystems. Old cache was 256 sectors, write-through, and its invalidate function was dead code.
- Partition parsing: MBR **and GPT**; never silently fall back to LBA 0 when no partition is found (old behavior mounted the MBR sector as a FAT boot sector).

### 7.2 Drivers

- **AHCI**: port from old tree onto the new block API (best existing driver).
- **NVMe**: written from scratch — admin queue creation, controller enable (`CC.EN` + `CSTS.RDY` handshake — the old driver never set the enable bit), I/O queue pair, PRP lists, MSI-X completion. The old driver returned pattern data on read and success on discarded writes; **nothing ships until it round-trips real data in QEMU**.
- **RAM disk**: with mathematically valid FAT geometry when self-formatting (old one sized the FAT at 32 sectors for a disk needing ~256, corrupting cluster chains beyond the window).

### 7.3 VFS

- vnode-based: `vnode` ops (lookup, read, write, create, unlink, rename, truncate, readdir-iterator, stat), dentry/name cache, per-mount instance data — **multiple mounts of the same fs type** (old tree had one global `fat32_filesystem` and one global `ext4_fs` singleton each).
- fd table per process; `O_CREAT/O_TRUNC/O_APPEND` semantics implemented in VFS, not ad-hoc per adapter.
- `rename` in the contract from day one (old `vfs_rename` was `-ENOSYS`, and `link/symlink/readlink` too).
- **Permissions enforced at open/access time** in VFS (`perm_check_access` existed in the old tree and was never called by any syscall). If v1 is single-user, say so in the docs — don't ship decorative security.
- All consumers go through VFS: installer, package manager, shell. No direct `fat32_*` calls from features (old installer bypassed VFS entirely).
- Streaming `readdir` with an iterator/cookie — never "materialize the whole directory into a kalloc'd array."

### 7.4 Filesystems

- **FAT32 (read/write)** first: mirrored FAT copies on every update (old tree wrote only FAT #1), backup boot sector on format, LFN on *create* as well as lookup (old tree wrote only 8.3 names, breaking its own LFN matching later), free-cluster summary in FSInfo, no whole-FAT-in-RAM requirement (cache FAT sectors through the buffer cache).
- **ext2 (read/write)** second: full indirect-block chains, directory entry compaction on delete, symlinks. Deliberately chosen over ext4-write because the old ext4 write path allocated blocks without inserting extents — silent corruption on any Linux-formatted volume. ext4 *read-only* is a post-v1 candidate.
- Lock scope: filesystem mutex protects metadata; **never held across block I/O when avoidable** (prepare, drop, submit, re-acquire); definitely no nested self-locking (the old `fat32_remove_empty_directory` deadlocked on its own non-recursive lock).
- `sync`/`fsync` plumbed from syscall → VFS → fs → bdev flush with correct ordering (old ext4_sync ignored write errors and never flushed the device).

**Exit criteria:** fsck (host-side) passes on images after a scripted torture run (create/write/rename/delete thousands of files, then hard-kill QEMU mid-run and verify no cross-linked clusters); both filesystems mount volumes created by Linux tools; NVMe and AHCI round-trip verified with checksummed data.

---

## 8. Phase 4 — Networking stack

**Duration:** 3–4 weeks.

### 8.1 Core architecture

- **`netbuf`** (skbuff-like): refcounted buffer with headroom for headers, owned explicitly at every hand-off. Drivers RX into netbufs; no more per-layer memcpy chains and no callback pointers into stack arrays.
- **One RX path**: driver IRQ → netbuf onto a lockless (or spinlock-guarded) input queue → dedicated net worker thread demuxes. **The TX path must never process RX inline** — the old `ip_send_packet` busy-polled the NIC and recursively ran `ethernet_process_frame` from inside sends, which combined with locks held by callers was a re-entrancy minefield.
- Lock model per protocol, honoring the Phase 0 rule: prepare segment under the connection lock, transmit after releasing it. (Root fix for the old tree's #1 network bug: `tcp_lock` held across `tcp_send_packet` in RX, send, *and* retransmit paths — guaranteed deadlock on a non-recursive spinlock.)

### 8.2 Sockets — one API

- `socket/bind/listen/accept/connect/send/recv/close` as the *only* interface, blocking by default on wait queues, `O_NONBLOCK` supported.
- One listener registry. The old tree had two incompatible listen mechanisms (callback `tcp_listen` used by telnet vs. syscall `tcp_socket_listen` that never registered a listener — inbound SYNs to syscall-bound ports got RST), and its `accept` grabbed *any* ESTABLISHED connection on the port.
- UDP sockets included (the old syscall layer was TCP-only); handler table replaced with a hash of bound sockets (the old one was a 512 KiB static array of 65536 function pointers, unlocked).

### 8.3 TCP

Written against the RFCs it claims, in this order:
1. Correct full state machine including FIN_WAIT_2 and a real TIME_WAIT (the old active close destroyed the connection immediately).
2. RTT measurement (RFC 6298: SRTT/RTTVAR actually updated — the old fields existed and were never written, so RTO was a constant).
3. Retransmission queue + exponential backoff (port concepts from old code, which had the structure right).
4. Randomized ISN (old seed was a hardcoded `0x12345678` — predictable and a security issue).
5. Reno congestion control; out-of-order segment queue (old tree dropped everything out of order).
6. Checksums always verified — no "skip validation if the driver didn't report a sane IP length" path.

### 8.4 Supporting protocols

- **ARP**: validated headers (htype/plen/opcode), cache with `deadline_ns` expiry through the single time API (the old expiry math made entries immortal), **pending-packet queue** so the first send to a host doesn't fail while resolution is in flight (old code returned failure and made callers poll).
- **IPv4**: reassembly with an actual cleanup timer wired into the net thread (the old `ip_fragment_cleanup` had zero callers) and RFC 5722 overlap-drop.
- **ICMP**: checksum computed in network byte order like every other protocol (old implementation summed host-endian words).
- **DHCP**: port the old client logic (its best network code) as a blocking task in the net thread; add NAK handling.
- **DNS**: per-request state (no single global `dns_state` corrupted by concurrent lookups), strict bounded label parsing (the old parser could jump unbounded on malicious lengths), random ephemeral ports.
- **No hardcoded network config in drivers.** IP/MAC/gateway comes from DHCP or the settings store, period. (Old E1000/RTL8139/PCnet each hardcoded `192.168.56.x` and a fallback MAC, and each re-ran `arp_init/ip_init` on top of kernel init.)

### 8.5 Drivers

- **virtio-net** first (transport already exists from virtio-gpu work; the old tree inexplicably never wrote the net device).
- **E1000** second: port register knowledge; interrupt-driven TX completion (no TX busy-wait spin), RX ring refill from netbuf pool.

**Exit criteria:** survives an `iperf`-style flood and a `nmap` scan from the host without deadlock, leak, or crash; wget-style HTTP GET of a large file with induced packet loss (qemu netem) completes correctly; concurrent DNS lookups from multiple processes; telnet server (now a *userspace* socket program) handles multiple sessions.

---

## 9. Phase 5 — Input and USB

**Duration:** 2–3 weeks.

### 9.1 Unified input layer

- One evdev-like event queue: `{type, code, value, timestamp_ns}` for key/rel/abs/button events. Console, compositor, and SDL shim are all *consumers* of this single stream — replacing the old three parallel keyboard paths (VC buffer + echo queue + SDL defer queue, each with its own duplicated backspace/Ctrl-C logic, one of which handled backspace with a double-decrement bug).
- Ctrl+C routing decided in exactly one place (the focused consumer), fixing the old split where PS/2 targeted the active VC but the USB path set a global flag on possibly the wrong console.

### 9.2 PS/2

- Full scancode set 1 with `0xE0` extended prefixes (old driver ignored them — arrow keys/right-ctrl broken on many keyboards).
- IRQ-driven only. The controller status AUX bit (bit 5) is *always* honored — the old `mouse_poll` fallback deliberately drained bytes "liberally" whenever OBF was set, eating keyboard bytes and desyncing mouse packets. No polling fallback that can steal another device's data; if IRQs are broken, fix the IOAPIC routing.

### 9.3 USB / xHCI

One stack, one owner (the old tree had `usb.c` scanning the wrong PCI subclass — 0x05 instead of 0x03 — finding nothing, while `xhci.c` enumerated devices into a private array that never reached the `usb.c` device lists the pollers iterated; and `usb_hid_read_report` zero-filled its buffer "for now"):

1. xHCI controller bring-up (port from old code — the ring/command handling was largely real).
2. Enumeration creates first-class device objects: descriptors fetched via real control transfers, interface classes parsed honestly (the old code labeled *every* interface-class device a "HID keyboard").
3. HID boot-protocol keyboard and mouse via **interrupt endpoints with event-ring completions** — no fake polling.
4. HID reports translate into the unified input layer.

**Exit criteria:** QEMU with `-device usb-kbd -device usb-mouse` (and PS/2 disabled) is fully usable in console and desktop; keyboard/mouse hotplug works; PS/2 and USB devices coexist without stealing each other's bytes.

---

## 10. Phase 6 — Graphics and desktop

**Duration:** 4–6 weeks.

### 10.1 Display backend

- `display_dev` interface with **honest capabilities**: a software pixel loop is not `GPU_CAP_2D` (the old HAL flagged software fills as GPU acceleration and the SDL renderer printed "GPU accelerated" for them).
- Backends: Limine linear FB (always) and virtio-gpu. Virtio completions **IRQ-driven** — the old driver busy-polled up to 1,000,000 iterations per flush. Keep and formalize the old virtio-vga "live scanout aliasing" trick (Limine FB backed by the device BAR) since it eliminates a full copy; make the non-aliased path equally correct instead of QEMU-config-dependent.

### 10.2 Compositor

- Runs as a **normal preemptible kernel thread** (possible now thanks to Phase 2) — not a call inside the kernel main loop.
- Port the old design's good ideas: per-window XRGB buffers, Z-order list, damage-union recomposite, dirty-row present, per-window fast path for fullscreen apps. Fix the known perf holes: no full-screen recomposite on every mouse-button transition; single present copy (compose into the scanout-backed buffer where the aliasing model allows).
- **Window lifetime contract** (Phase 0 ownership rules applied): the close button sends `GUI_EVENT_CLOSE` to the owning process; the kernel frees window buffers only when the process explicitly destroys the window or dies. This permanently fixes the old close-button UAF where the kernel freed pixels the client SDL surface still pointed at, then both sides could double-free.
- One lock hierarchy for window state; no unlocked list getters (`desktop_get_focused` reading during concurrent destroy was an SMP race).

### 10.3 SDL shim

- Keep the in-kernel SDL compatibility layer (it's what makes DOOM and ported apps work) as a thin adapter over: compositor windows, the unified input queue (real event delivery, not empty "inject" stubs), and the single time API (`SDL_GetTicks` from `time_ms()` — the old shim hardcoded a 14.318 MHz HPET frequency and admitted in a comment it never read the real one). `SDL_WaitEvent` blocks on a wait queue instead of `Delay(1)` spinning.

### 10.4 Image decoding

- Replace the hand-rolled PNG/DEFLATE decoder with a vetted, size-bounded single-file library (stb_image-class), fuzzed on the host before inclusion. The old decoder skipped chunk CRCs entirely, never verified zlib Adler-32, silently rewrote invalid color types instead of rejecting the file, bounded its filter pass with a hardcoded 10 MiB constant instead of the actual buffer size, and didn't support interlacing or palettes. In-kernel parsing of untrusted files is exactly where that's unacceptable.

**Exit criteria:** desktop runs with ≥3 concurrent client apps; close/kill/crash any client at any moment with zero UAF (verified with heap poisoning in debug builds); DOOM port at stable frame rate; wallpaper loads arbitrary well-formed PNGs and cleanly rejects a corpus of malformed ones.

---

## 11. Phase 7 — Userland, shell, and modules

**Duration:** 2–3 weeks.

### 11.1 Minimal libc + coreutils

- `user/libc-min`: crt0, syscall wrappers, string/mem, stdio-lite (buffered fd wrappers), malloc over `mmap/brk`.
- Real coreutils built by the main Makefile as user ELFs: `sh`-compatible entry, `ls`, `cat`, `echo`, `mkdir`, `rm`, `cp`, `mv`, `ps`, `kill`. (The old `apps/ls.c` etc. included kernel headers and were never buildable as user programs; `cp`/`mv` in the old kernel shell printed "not fully implemented".)

### 11.2 Zeus in userspace

- Port Zeus (history, line editing, pipes, redirection, job basics) onto the real syscall ABI. Pipelines now genuinely work because pipes block and `wait` blocks. The kernel keeps only a tiny emergency debug console on serial.

### 11.3 Kernel modules (optional for v1)

If shipped, fixed properly; otherwise stay monolithic:
- Loader **fails closed** on any unresolved symbol (the old kmod and dynlink both logged and "continued anyway", leaving zero pointers to fault at runtime).
- Sections mapped with correct permissions — RX text, RW data, W^X enforced (old loader dropped everything into one RWX kalloc blob).
- Full x86-64 relocation set including GOT/PCREL forms; explicit export table; refcounted unload.
- Driver "manager" reduced to what's real: a registry binding one driver **instance per PCI function** (the old one silently skipped the second identical NIC) with init/deinit that actually touch hardware. No save-to-disk facades.

**Exit criteria:** boot → login-less Zeus in userspace → pipelines, redirection, and job control over real coreutils; a demo module loads/unloads repeatedly without leaks (if modules ship).

---

## 12. Testing strategy

Testing is what the old tree lacked entirely (a `tests/` folder of demo programs is not a test suite).

| Layer | Approach |
|---|---|
| Host unit tests | `lib/`, allocators, TCP state machine, path parsing, PNG rejection corpus — compiled for the host with mocks, run in CI on every commit |
| Kernel self-tests | `make qemu-test` boots QEMU headless, runs a self-test binary (fork/wait torture, COW checks, pipe semantics, FS torture), reports pass/fail over serial with an exit device |
| Crash consistency | Scripted hard-kill of QEMU mid-write, then host-side fsck of the disk image |
| Network conformance | Host-driven: scapy scripts throwing malformed/fragmented/out-of-order packets; nmap; packet-loss HTTP transfer |
| Fuzzing | Host-side fuzz of PNG decoder, DNS parser, TCP option parsing, FAT32 directory parsing before each is enabled in-kernel |
| Debug builds | Slab redzones + poisoning, lock-order assertions, `KASSERT` on all invariants |

**Definition of done for any phase:** its exit criteria pass in CI, and no code in the tree returns fake success, contains a "for now" that changes behavior, or busy-waits outside arch bring-up.

---

## Timeline summary

| Phase | Content | Estimate |
|---|---|---|
| 0 | Scope, contracts, skeleton, CI | 1–2 wk |
| 1 | PMM/slab/VMM, per-CPU, interrupts | 3–5 wk |
| 2 | Scheduler, wait queues, processes, signals, pipes | 3–5 wk |
| 3 | Block layer, AHCI/NVMe, VFS, FAT32, ext2 | 3–4 wk |
| 4 | netbufs, sockets, TCP/UDP/ARP/DHCP/DNS, virtio-net/E1000 | 3–4 wk |
| 5 | Input layer, PS/2, xHCI/USB HID | 2–3 wk |
| 6 | Display backends, compositor, SDL shim, image decoding | 4–6 wk |
| 7 | libc-min, coreutils, userspace Zeus, (modules) | 2–3 wk |
| **Total** | | **~6–8 months part-time** |

Phases 3–5 have limited interdependencies and can partially overlap once Phase 2 lands. Phases 1–2 are strictly sequential and are the ones that determine whether this rewrite escapes the old codebase's fate: every entry in Appendix A traces back to a missing Phase 0–2 foundation, which forced each subsystem into "poll it, fake it, or return success."

---

## Appendix A — Stub/fake inventory of the old codebase

Everything below *looked* functional in the old tree but was simulated, hardcoded, or empty. None of it carries into the rewrite as-is.

| Component | Reality | Reference |
|---|---|---|
| NVMe driver | Reads return `(lba^0xDEADBEEF)` pattern; writes logged and discarded; controller enable bit never set; function signature mismatched `storage.c` caller (UB) | `nvme.c:115-242`, `storage.c:484` |
| USB HID input | `usb_hid_read_report` zero-fills "for now"; xHCI HID poll is an empty placeholder; xHCI enumeration never populates `usb.c` device lists | `usb.c:339`, `xhci.c:895` |
| `usb_init` PCI scan | Scans subclass 0x05 instead of 0x03 — finds zero USB controllers | `usb.c:508` |
| USB mass storage | BOT unimplemented; read/write always fail | `usb.c:241-269` |
| VMware SVGA | Every function stubbed; `is_available()` hardcodes false; still registered and branched on | `vmware_svga.c:43-100` |
| AMD GPU | Fake MMIO "command registers" at invented offsets + software VRAM loops; ROM read after PCI ROM disable | `amd_gpu.c:52-198` |
| AtomBIOS interpreter | Handles NOP and EOT only | `atom_bios.c:45-54` |
| DRM | Only VERSION/GET_CAP implemented; advertises dumb-buffer cap it doesn't have; `/dev/dri` read/write/readdir return -1 | `drm.c:81-137`, `dev_dri_vfs.c:41-89` |
| Driver manager persistence | save-to-disk / load-from-memory print "not implemented"; `driver_file_format.h` has no `.c`; adapter deinit/disable/reset flip booleans | `driver_manager.c:787-800`, `driver_adapters.c` |
| Signals | `sigaction` stores user handlers never invoked; delivery terminates the process; no sigreturn | `syscall.c:1474-1500`, `2242` |
| Pipes | Return 0 ("would block in a real OS") when empty or full | `pipe.c:96-129` |
| `sys_wait` | Returns `-ECHILD` immediately if no zombie exists | `syscall.c:1401` |
| `sys_exec` argv | `(void)argv;` — arguments discarded; user stack hardcodes argc=1 | `syscall.c:1592`, `exec.c:846` |
| SMP | APs park in `hlt`; scheduler returns early on non-BSP; single `bsp_gs`; LAPIC-ID-vs-index bug in AP wake | `smp.c:186`, `idt.c:795`, `syscall.c:60-65`, `cpu.c:67-72` |
| IPv6 L4 | "TCP/UDP over IPv6 not yet supported" — but v6 frames still demuxed | `net_ipv6.c:509-514` |
| RTL810x NIC | Marks itself initialized; no RX/TX | `rtl810x.c:69-76` |
| virtio-net | Doesn't exist (transport only) | `virtio.c` |
| Hardcoded net config | E1000/RTL8139 `192.168.56.2`, PCnet `.3`, fallback MAC `52:54:00:12:34:56`, RTL8139 IRQ 11 assumed | `e1000.c:325`, `rtl8139.c:582-620`, `pcnet.c:207` |
| ext4 htree | Claimed in header, absent from code | `ext4.h:9` |
| ext2 | Read-only, never wired into VFS — dead code | `ext2.c` |
| `vfs_rename`/`link`/`symlink`/`readlink` | `-ENOSYS` | `vfs.c:340`, `syscall.c:2205-2221` |
| Permissions | `perm_check_access` implemented, never called by any syscall | `users.c:428`, `syscall.c` |
| PIT timer | Initialized, IRQ handler commented out; `get_ticks()` always 0 | `timer.c:13-16` |
| libc shim time | `time()`/`clock()` return 0 | `libc_shim.c:78,298` |
| Debug heap checker | printf-only stub; allocation tracking never wired into kalloc | `debug.c:80-84` |
| SDL audio / event injection | Not implemented / empty functions | `sdl_main.c:88`, `sdl_integration.c:65-78` |
| SDL timer | Hardcodes 14.318 MHz HPET frequency | `sdl_timer.c:33` |
| Luade `gfx.draw_text` | No-op | `luade_graphics.c:288-301` |
| `apps/ls.c`, `cat.c`, `echo.c`, … | Include kernel headers; never build as user programs | `apps/` |
| `cmd.c` copy/move | "not fully implemented" | `cmd.c:505-515` |
| kmod/dynlink unresolved symbols | Logged, load "continues anyway" with zeroed pointers | `kmod.c:432`, `dynlink.c:172` |
| Flat-binary "relocation fixer" | Heuristically patches immediates < 0x1000 — can corrupt valid code | `exec.c:164-191` |
| `NET_GET_STATUS` | Hardcoded 1 if device exists | `syscall.c:2723` |
| bcache invalidate | Defined, never called | `storage.c:141` |

## Appendix B — Worst bug classes in the old codebase

Grouped by the root cause the rewrite eliminates:

**Locking model (Phase 0/2):** TCP deadlock — non-recursive `tcp_lock` held across sends in RX/send/retransmit paths (`net_tcp.c:970+`); FAT32 self-deadlock in `fat32_remove_empty_directory` (`fat32.c:1728,1748`); unlocked readers of window list, driver list, ARP cache, UDP handlers, clipboard, ext4 globals; `ide_write_sectors` missing the lock its read path takes.

**Ownership (Phase 0):** desktop close-button frees `win->pixels` while the client SDL surface still points at it — UAF + double-free (`desktop.c:1717`, `sdl_video.c:167`); TCP data callbacks receive pointers into stack RX buffers (`net_tcp.c:968+`); `thread_exit` never frees stack/TCB (`thread.c:178`); user-fault kill path leaks the whole process (`idt.c:687`).

**Time units (Phase 0):** ARP entries immortal, TCP TIME_WAIT never fires — ms values re-divided by HPET frequency (`net_arp.c:160`, `net_tcp.c:307`).

**Blocking primitives (Phase 2):** fake pipes, non-blocking `wait`, vfork-in-disguise `fork` with a conflicting slot allocator (`syscall.c:1183,1322`), TCP connect returning pre-ESTABLISHED, two incompatible TCP listen registries with `accept` grabbing any ESTABLISHED connection on the port (`net_tcp.c:898-1248`).

**Atomicity (Phase 1):** COW refcount decremented non-atomically in the #PF handler bypassing the CAS helpers (`idt.c:299`); `uint8_t` refcounts saturating into permanent leaks (`mem.c:1093`); mempool with no locking (`mem.c:1185`).

**Bounds/validation (Phase 0 fail-closed):** `pci_devices[32]` unchecked overflow (`pci.c:207`); PNG CRC skipped + hardcoded 10 MiB filter bound + invalid color types silently rewritten (`png.c:96,1042,1109`); DNS label parser unbounded jumps (`net_dns.c:174`); ARP header fields unvalidated; TCP checksum skipped when IP length "not sane" (`net_tcp.c:874`); RAM disk FAT sized 32 sectors where ~256 needed (`storage.c:297`); ext4 extent-write corruption (`ext4.c:544`); FAT mirror #2 never written (`fat32.c:1336`); MBR-miss falls back to LBA 0 (`storage.c:528`).

**Input concurrency (Phase 5):** `mouse_poll` drains keyboard bytes when the AUX bit is unset (`mouse.c:431`); backspace double-decrement (`keyboard.c:444`); `0xE0` scancodes ignored; USB Ctrl+C targets the wrong console.

## Appendix C — Performance anti-patterns in the old codebase

Do not reproduce any of these:

- **Polling as architecture:** kernel main loop polls mouse + (empty) USB + compositor + net fallback every iteration; virtio-gpu busy-polls ≤1M iterations per flush; E1000 TX spins; `SDL_WaitEvent` spins on `Delay(1)`; ARP/DNS/DHCP busy-wait while re-entering the RX stack from send paths.
- **Global locks across I/O:** `vfs_lock` and FAT32 lock held for entire read/write including disk access; `desktop_lock` held during panel text rendering.
- **O(n) hot paths:** bitmap first-fit scan per kalloc with IRQs off; full directory re-read per path component; entire FAT loaded at mount; linear scans for threads/processes/drivers/symbols; full 256×32 PCI config scan at boot.
- **Copy amplification:** up to triple framebuffer copy per composited frame (backbuffer → Limine FB → HAL surface → flush); per-packet memcpy at every network layer; Luade buffer swap via thousands of `fillrect` calls.
- **Log storms on hot paths:** `kalloc_high` printf per allocation; TCP send and IP route `serial_printf` per packet; AMD putpixel printf per call.
- **Memory waste:** 512 KiB static UDP handler array; 1920×1080×4 static Luade pool; page-granular allocation for every small object.
