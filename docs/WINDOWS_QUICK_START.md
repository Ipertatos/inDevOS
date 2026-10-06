# inDevOS Windows Quick Start Guide

## Read this first: the toolchain is clang + lld, not MinGW

MinGW-w64 **cannot** build this kernel:

* MinGW's `ld` only implements the `i386pe`/`i386pep` (PE/COFF) emulations, so
  it cannot link the ELF64 kernel produced by `linker.ld` and required by the
  Limine boot protocol -- the link fails with "unsupported emulation".
* MinGW's `gcc` emits COFF objects, not the ELF64 objects the kernel needs.

The supported Windows toolchain is **LLVM**:

| Tool | Used for |
| --- | --- |
| `clang --target=x86_64-unknown-none-elf` | compiling the kernel and Lua to ELF64 |
| `ld.lld` | linking the ELF64 kernel |
| `llvm-objdump` | generating `kernel.sym` (MinGW's `nm` cannot read ELF) |
| `nasm` | assembling `src/*.asm` |
| `xorriso` | building `os.iso` |
| `py`/`python` | generating the kernel symbol table |
| `qemu-system-x86_64` | running the ISO |

`build_windows.bat` and the MinGW instructions below are kept for historical
reference only; they cannot produce a bootable kernel. Use
`build_windows.ps1` instead. On Linux or WSL, use the `Makefile` (`make
kernel_final.bin && make os.iso`).

## TL;DR - Get Started Fast

**Want to use cmd/PowerShell/Windows Terminal instead of MSYS2?** Follow these steps:

### 1. Download Tools (5 minutes)

1. **LLVM/clang**: `winget install LLVM.LLVM` (provides `clang`, `ld.lld`,
   `llvm-objdump`) -- *required*

2. **NASM**: Download from [nasm.us](https://www.nasm.us/pub/nasm/releasebuilds/)
   - Install Windows 64-bit version

3. **Python 3**: Download from [python.org](https://www.python.org/downloads/)
   - Check "Add Python to PATH"

4. **QEMU**: Download from [qemu.org](https://www.qemu.org/download/#windows)
   - Windows installer

### 2. Add to PATH (2 minutes)

Add these to your Windows PATH environment variable:
- `C:\Program Files\LLVM\bin` (clang, ld.lld, llvm-objdump)
- `C:\Program Files\NASM` (or wherever NASM installed)
- `C:\Program Files\qemu` (or wherever QEMU installed)

**How to add to PATH:**
1. Windows key → type "environment" → "Edit system environment variables"
2. Click "Environment Variables" → Edit "Path" 
3. Add the directories above
4. Restart cmd/PowerShell

### 3. Test Installation (1 minute)

Open cmd or PowerShell and run:
```cmd
clang --version
ld.lld --version
nasm -v
py -3 --version
qemu-system-x86_64 --version
```

All commands should work without errors.

### 4. Build inDevOS (2 minutes)

```powershell
cd C:\path\to\your\inDevOS
.\build_windows.ps1                 # kernel_final.bin + os.iso
.\build_windows.ps1 -Clean          # full rebuild
```

The script verifies up front that the compiler can emit ELF64 and that the
linker accepts it, and reports the exact path of each tool it found (`-Clang`,
`-Lld`, `-Nasm`, `-Xorriso`, `-Qemu` override the auto-detection).

### 5. Run inDevOS (30 seconds)

```powershell
.\run_windows.bat
# or build + boot + dump the serial log in one go:
.\build_windows.ps1 -BootTest
```

**Done!** Your OS should boot in QEMU.

---

## File Guide

| File | Purpose | When to Use |
|------|---------|-------------|
| `build_windows.ps1` | **Supported Windows build** (clang + lld) | Always |
| `build_wsl.sh` | Build + headless boot test from WSL | WSL users |
| `Makefile` | Canonical Linux/WSL/CI build | Linux, WSL, CI |
| `run_windows.bat` | Run the built `os.iso` in QEMU | Every time you test |
| `build_windows.bat` | *Obsolete MinGW build* -- cannot link the ELF64 kernel | Historical only |
| `Makefile.windows` | *Obsolete MSYS2 Makefile* -- same limitation | Historical only |
| `MINGW_SETUP.md`, `WINDOWS_SETUP.md`, `WINDOWS_NATIVE_SETUP.md` | *Obsolete MinGW setup guides* | Historical only |
| `TROUBLESHOOTING_WINDOWS.md` | Fix problems | When things break |

### Run a headless boot smoke test

```powershell
.\build_windows.ps1 -BootTest            # 60 s boot, serial log -> serial-win.log
.\build_windows.ps1 -Target boot         # just boot the existing os.iso
```

The kernel prints a `[ N.NNNNNN]` timestamp on every line, so the serial log can
be diffed between builds to see exactly what a change did.


## Commands Quick Reference

### Building
```cmd
REM Full clean build
build_windows.bat clean
build_windows.bat

REM PowerShell version (advanced)
.\build_windows.ps1 -Clean

REM Just build kernel (no ISO)
.\build_windows.ps1 -Target kernel
```

### Running
```cmd
REM Standard run
run_windows.bat

REM Manual QEMU command
qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log
```

### Debugging
```cmd
REM View serial output while running
type serial.log

REM Monitor serial output in real-time (PowerShell)
Get-Content serial.log -Wait
```

## Common First-Time Issues

**"gcc not found"**
→ Add MinGW-w64 to PATH and restart terminal

**"Build failed"** 
→ Run from project root directory, check PATH

**"QEMU failed"**
→ Install QEMU and add to PATH

**"Permission denied"**
→ Run as Administrator or check antivirus

**"xorriso not found"**
→ Kernel builds fine, just can't create ISO. Install xorriso or ignore.

## What You Get

✅ **Native Windows development** - Use your preferred Windows terminal
✅ **Full compatibility** - Existing Makefile logic preserved  
✅ **Better IDE support** - Works great with VS Code, CLion, etc.
✅ **No Unix layer** - Direct Windows execution
✅ **Small footprint** - Only ~200MB vs 2GB for MSYS2

## Alternative: Still Want MSYS2?

If you prefer the Unix-like environment, see `WINDOWS_SETUP.md` for the original MSYS2 approach.

---

**Questions?** Check `TROUBLESHOOTING_WINDOWS.md` for detailed solutions to common problems.

























