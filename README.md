# inDevOS

A hobby x86-64 operating system written in C and assembly, booted by Limine
(BIOS and UEFI). It runs its own shell (Zeus), a Lua-driven desktop (LuaDE),
and unmodified static Linux x86-64 binaries such as BusyBox, TinyCC and curl.

Still early development; expect rough edges.

## Features

- **Core:** SMP, APIC/IOAPIC, HPET/LAPIC timers, RTC, ACPI tables, PCI/PCIe
  (ECAM), paging with copy-on-write, kernel threads and preemptive scheduling.
- **Processes:** real COW `fork`/`vfork`/`clone`, `execve`, `wait` and zombies,
  process groups, POSIX signals (Ctrl-C to the foreground group, `kill(-pgid)`,
  `SA_RESTART`/`EINTR`), pipes, deferred safe remote kill.
- **Linux ABI:** a Linux-compatible x86-64 syscall layer (files, mmap/brk,
  futex, poll, eventfd, sockets, ioctls, `/dev/fb0`). Static **musl** binaries
  run unchanged: **BusyBox** (`ash` and applets), **TinyCC** (compiles and runs
  C on the OS, including itself), **curl** (HTTP/HTTPS).
- **Storage:** AHCI (SATA), IDE/ATAPI, **NVMe** (multi-controller, 4K LBA),
  USB mass storage; MBR and **GPT**; FAT32 (LFN, case-preserving) and ext2/ext4
  behind a VFS.
- **USB:** xHCI with hubs and hotplug, `lsusb`, **HID** keyboard and mouse,
  **mass storage** (BOT/SCSI), **USB Ethernet** (CDC-ECM, RNDIS; NCM untested).
- **Networking:** RTL8139, E1000, PCnet and USB Ethernet; Ethernet/ARP/IPv4/
  ICMP/UDP/TCP, DHCP, DNS, ping, telnet server.
- **Graphics and desktop:** framebuffer console (flanterm), virtual terminals,
  software compositor with LuaDE desktop and a desktop terminal, write-combined
  framebuffer, Linux fbdev **`/dev/fb0`** (mode ioctls + `mmap`) so framebuffer
  programs can take over the screen; virtio-gpu.
- **Audio:** AC'97, PC speaker.
- **Install:** the in-OS `install` command installs to a disk with an MBR or
  **GPT/UEFI** layout; a bootable install image (`os_install.iso`) carries the
  root filesystem.
- **DOOM:** a port of linuxdoom-1.10. The game data is **not** included; supply
  your own `doom1.wad` (the shareware WAD is freely available).

## Real hardware

inDevOS boots from a USB stick on an AMD Ryzen (APU) laptop with the USB
keyboard working. A USB Ethernet adapter, plugged in through a hub, gets an
address via DHCP, and `ping` and `curl` work from there. AMD GPU
acceleration is not implemented (firmware framebuffer only).

## Building and running

### Linux / WSL (canonical build)

```sh
make -C limine                 # build the Limine host tool (once)
make kernel_final.bin -j"$(nproc)"
make os.iso
make run                       # needs sudo + a tap device for guest networking
```

Requires `gcc make nasm xorriso qemu-system-x86 python3`. If a bare-metal cross
compiler exists at `$HOME/opt/cross/bin/x86_64-elf-gcc` the Makefile uses it;
otherwise the host `gcc` compiles the kernel in freestanding mode.

The kernel links **no host C library**. Everything it uses from `<string.h>`,
`<math.h>`, `<setjmp.h>`, ... is implemented in-kernel (`mem.c`, `string.c`,
`libc_shim.c`, `asmutils.asm`) and declared by the freestanding headers in
`src/libc/` -- see `src/libc/README.md`.

### Windows (PowerShell)

MinGW-w64 **cannot** build this kernel: its `ld` only implements the PE/COFF
emulations, so it cannot link the ELF64 kernel that `linker.ld` and Limine
require, and its `gcc` emits COFF objects rather than ELF64. Use clang + lld:

```powershell
.\build_windows.ps1                 # kernel + os.iso
.\build_windows.ps1 -Clean          # full rebuild
.\build_windows.ps1 -Target boot    # headless QEMU boot smoke test
.\build_windows.ps1 -BootTest       # build, then boot, then dump the serial log
```

Requires LLVM (`clang`, `ld.lld`, `llvm-objdump`), NASM, Python 3, QEMU and
xorriso (for `os.iso`). See [`docs/WINDOWS_QUICK_START.md`](docs/WINDOWS_QUICK_START.md) for install steps.

### Install ISO (bootable rootfs for a PC or VM)

os.iso is only the kernel and Limine. The kernel mounts a FAT32 volume as /
(raw superfloppy, or the first MBR partition of type 0x0B/0x0C). One command builds
os_install.iso: the same Limine layout as os.iso, plus that FAT32 root as a
partition, with the real /bin tools and Doom (no test or demo apps).

```powershell
.\make_install_iso.ps1
```

Same thing: make install-iso (WSL/Linux) or .\build_windows.ps1 -Target install-iso.
Needs WSL with mtools and xorriso (already how apps/build_apps.sh fills sata_disk.img).

Boot the image as a hard disk or USB stick (Rufus or dd in DD mode, or QEMU
-drive file=os_install.iso,format=raw). A lone -cdrom starts Limine, but this
kernel has no ISO9660 driver, so it will not see the rootfs unless the image is
the storage device it mounts. The in-OS install command copies only the kernel
and Limine from /install/; it does not clone /bin or Doom onto another drive.

### Tests and helper scripts

One-off QEMU verification and benchmark scripts live in `scripts/`. They find the
repo from their own location; override with `OSPROJ=...`. Log/scratch locations
default to your Windows profile under WSL (`WINHOME=...` to override).
`qmp_step.py` / `qmp_scenario.py` (repo root) drive QEMU over QMP.

## Licence

inDevOS is free software under the **GNU General Public License, version 2 or
(at your option) any later version**; see [`LICENSE`](LICENSE). Bundled and
related third-party components and their licences are listed in
[`THIRD_PARTY.md`](THIRD_PARTY.md).

## Credits / inspiration

Desktop environment and window-manager ideas (event polling, compositor layering, dock-style launcher patterns) were informed by studying the public [BoredOS](https://github.com/BoredDevNL/BoredOS) project documentation and architecture; the inDevOS implementation is original code written for this kernel.
