# inDevOS Windows Native Development Setup

## Overview

This guide sets up inDevOS development using native Windows tools (cmd/PowerShell/Windows Terminal) instead of MSYS2. This approach uses Visual Studio Build Tools and Windows-native versions of required tools.

## Prerequisites

### 1. Install Visual Studio Build Tools 2022

Download and install **Visual Studio Build Tools 2022** from:
https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022

During installation, select:
- **C++ build tools** workload
- **Windows 10/11 SDK** (latest version)
- **CMake tools for C++** (optional but recommended)

### 2. Install Required Tools

#### QEMU (x86_64 emulator)
- Download from: https://www.qemu.org/download/#windows
- Add QEMU installation directory to Windows PATH
- Default location: `C:\Program Files\qemu`

#### NASM (Netwide Assembler)
- Download from: https://www.nasm.us/pub/nasm/releasebuilds/
- Download the Windows 64-bit installer (`nasm-X.XX.XX-installer-x64.exe`)
- Add NASM to Windows PATH (usually `C:\Program Files\NASM`)

#### Python 3
- Download from: https://www.python.org/downloads/windows/
- **Important**: Check "Add Python to PATH" during installation
- Required for symbol table generation

#### Git for Windows
- Download from: https://git-scm.com/download/win
- Choose "Use Git from the Windows Command Prompt"

#### xorriso (ISO creation)
- Download from: https://www.gnu.org/software/xorriso/
- Or use the Windows version from MSYS2 packages without installing full MSYS2
- Alternative: Use `mkisofs` from cdrtools

## Environment Setup

### 1. Open Visual Studio Developer Command Prompt

You have several options:
1. **Start Menu**: "Developer Command Prompt for VS 2022"
2. **Start Menu**: "Developer PowerShell for VS 2022" 
3. **Windows Terminal**: Run `cmd` then execute:
   ```cmd
   "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
   ```

### 2. Verify Tool Installation

```cmd
REM Check compiler
cl /?

REM Check linker
link /?

REM Check assembler
nasm -v

REM Check QEMU
qemu-system-x86_64 --version

REM Check Python
python --version
```

## Project Setup

### 1. Extract Project
Extract your inDevOS backup to a development directory:
- Example: `C:\Dev\inDevOS`

### 2. Create Disk Images
```cmd
cd C:\Dev\inDevOS
qemu-img create -f raw sata_disk.img 512M
qemu-img create -f raw nvme_disk.img 1G
```

### 3. Directory Structure
Your project should look like:
```
C:\Dev\inDevOS\
├── src\                 # Source code
├── limine\             # Bootloader
├── third_party\        # Lua source
├── build_windows.ps1   # PowerShell build script
├── build_windows.bat   # Batch build script
├── run_windows.bat     # Windows run script
└── ...
```

## Build System

### Compiler Flags Translation

The project uses GCC-specific flags that need translation for MSVC:

| GCC Flag | MSVC Equivalent | Purpose |
|----------|-----------------|---------|
| `-ffreestanding` | `/kernel` | Kernel mode compilation |
| `-fno-pie` | N/A | Position independent code disabled |
| `-m64` | N/A | 64-bit target (default in x64 tools) |
| `-mgeneral-regs-only` | N/A | Restrict register usage |
| `-mcmodel=kernel` | N/A | Memory model for kernel |
| `-msse2` | `/arch:SSE2` | SSE2 instruction set |

### Build Challenges

1. **GNU LD vs MSVC Linker**: The project uses a custom linker script (`linker.ld`) that's GNU LD specific
2. **ELF Format**: MSVC produces PE files, but the project expects ELF format
3. **Cross-compilation**: Need x86_64-elf target

## Alternative Approach: Clang/LLVM

For better compatibility, consider using **Clang/LLVM** instead of MSVC:

### 1. Install LLVM
- Download from: https://releases.llvm.org/
- Add LLVM bin directory to PATH

### 2. Use Clang with GNU-compatible flags
```cmd
clang --target=x86_64-elf -ffreestanding -fno-pie -mcmodel=kernel ...
```

## Build Scripts

The following build scripts will be created:
- `build_windows.ps1` - PowerShell version with better error handling
- `build_windows.bat` - Batch file for basic cmd compatibility
- `run_windows.bat` - QEMU execution script

## Current Project Status

### Completed Features
- ✅ Kernel boots successfully with BSS clearing fix
- ✅ Lua Desktop Environment (LuaDE) integration  
- ✅ FAT32 filesystem support
- ✅ Basic PNG decoder with DEFLATE decompression
- ✅ `img` command for loading and displaying images
- ✅ Framebuffer graphics output

### Build Dependencies
- **Primary**: C compiler (GCC/Clang), GNU LD, NASM
- **Tools**: QEMU, xorriso, Python 3
- **Libraries**: Lua 5.4 (vendored in third_party/)

## Recommended Approach

Given the complexity of adapting the GNU toolchain dependencies, I recommend one of these approaches:

### Option 1: MinGW-w64 (Recommended)
Install MinGW-w64 standalone (without MSYS2):
- Download from: https://www.mingw-w64.org/downloads/
- Provides GCC, binutils, and GNU LD for Windows
- Maintains compatibility with existing Makefile

### Option 2: Windows Subsystem for Linux (WSL)
- Install WSL2 with Ubuntu/Debian
- Install development tools in Linux environment
- Access files from Windows while building in Linux

### Option 3: Full Windows Native Port
- Significant code changes required
- Replace GNU LD with custom linker or adapt for MSVC
- Port assembly syntax and calling conventions

## Next Steps

Based on your preference, I can:
1. Create MinGW-w64 setup instructions (easiest)
2. Create WSL2 setup instructions (Linux tools, Windows integration)
3. Begin full Windows native port (most complex, significant changes required)

Which approach would you prefer?

























