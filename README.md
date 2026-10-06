A simple operating system made in C and ASM

Currently working:
APIC, PIC, boot, ISRs/IRQs, HPET/LAPIC timers, RTC, keyboard+mouse, SMP,
PCI, storage (AHCI/IDE/NVMe), FAT32 + ext2/ext4 with a VFS, networking
(RTL8139/E1000, IP/UDP/TCP/DHCP/DNS, telnet), USB (xHCI), AC'97 audio,
framebuffer console + a Lua-driven desktop environment (LuaDE).

Still early development, needs time to get to a good working state before the
first polishing steps.

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
xorriso (for `os.iso`). See `WINDOWS_QUICK_START.md` for install steps.

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

## Credits / inspiration

Desktop environment and window-manager ideas (event polling, compositor layering, dock-style launcher patterns) were informed by studying the public [BoredOS](https://github.com/BoredDevNL/BoredOS) project documentation and architecture; the inDevOS implementation is original code written for this kernel.
