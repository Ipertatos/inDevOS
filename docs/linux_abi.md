# Linux x86-64 syscall ABI (phase 1: static musl)

inDevOS has two user ABIs side by side:

| Entry            | ABI                         | Used by                                   |
|------------------|-----------------------------|-------------------------------------------|
| `int $0x80`      | private inDevOS numbers     | apps/*.c via include/syscalls.h, Doom, sh |
| `syscall` instr. | Linux x86-64 numbers/structs| static musl (and later glibc) binaries    |

Any process that executes `syscall` is treated as a Linux process
(`process_t.linux_abi` is set and logged on its first call).

## Pieces

* `src/syscallasm.asm` `syscall_entry` - SYSCALL entry. swapgs only around the
  per-CPU kernel-stack load (IF=0), swapped back before any C code, so GS is
  always in its invariant state (GS_BASE=user, KERNEL_GS_BASE=per-CPU) outside
  the stub. Builds a `linux_syscall_frame_t` and returns with `sysret`
  (falls back to `iretq` if the saved RIP is non-canonical).
* `src/linux_syscall.c` - number dispatcher + adapters over the sys_* in
  syscall.c (flag/struct translation, dirfd + `.`/`..` path resolution).
  Unimplemented numbers return `-ENOSYS` and are logged to serial once.
* `src/exec.c` - ELF loader: static ET_EXEC (0x400000) and static-pie ET_DYN,
  segments sharing a page, PT_INTERP rejected with a message, and a full SysV
  initial stack (argc, argv, envp, auxv incl. AT_PHDR/AT_RANDOM/AT_HWCAP...).
* FS base (TLS) is saved/restored per thread on every context switch
  (`thread_switch_fs`, idt.c preempt path) and set by `arch_prctl(ARCH_SET_FS)`.

## Try it

    wsl bash tests/musl/build_musl_tests.sh      # builds + installs into sata_disk.img
    .\build_windows.ps1 -Target all              # kernel + os.iso
    .\make_install_iso.ps1                       # os_install.iso (/bin has the tests)

In the Zeus shell: `hello_musl` or `musltest` (or `/bin/hello_musl.exe`).

`hello_musl` works from the live DVD (`os_install.iso`). `musltest` file checks
(creat/unlink) need a **writable** FAT32 root: boot the install ISO, then
`mount 0` (or whichever device is the HDD — check `storage`) so `/` is the
writable disk that already has `/bin/hello_musl.exe` and `/bin/musltest.exe`
(from `tests/musl/build_musl_tests.sh` into `sata_disk.img`). On the DVD alone,
creat fails with "Failed to write directory entry" (read-only optical).
Stdout is already on the console serial path; set `linux_stdout_mirror = true` in src/linux_syscall.c to also echo the first 4 KB of fd 1/2 to serial (off by default to avoid double printing).

Debug knobs (src/linux_syscall.c): `linux_syscall_trace` (log every call),
`linux_stdout_mirror`.


## BusyBox

Static musl BusyBox lives at `/bin/busybox.exe` (also `tests/busybox/build/busybox.exe`).

    wsl bash tests/busybox/build_busybox.sh   # or rebuild_bb.sh
    wsl bash tests/busybox/install_busybox.sh # into sata_disk.img
    .\make_install_iso.ps1                    # packs into os_install.iso too

At the Zeus prompt (after `mount 0` if on the install DVD):

    busybox
    busybox echo hi
    busybox sh
