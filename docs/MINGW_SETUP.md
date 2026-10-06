# MinGW-w64 Standalone Setup for inDevOS

## Overview

This guide shows how to set up inDevOS development using MinGW-w64 standalone (without MSYS2) for cmd/PowerShell/Windows Terminal. This provides GNU tools while maintaining Windows-native workflow.

## Installation

### 1. Download MinGW-w64 Standalone

**Option A: WinLibs (Recommended)**
- Download from: https://winlibs.com/
- Choose: "MSVCRT runtime" → "Win64" → "GCC (latest stable)"
- Example: `winlibs-x86_64-posix-seh-gcc-XX.X.X-mingw-w64msvcrt-XX.X.X-r1.zip`

**Option B: Official MinGW-w64**
- Download from: https://www.mingw-w64.org/downloads/
- Choose a pre-built package (e.g., from MSYS2 or other providers)

### 2. Extract and Setup PATH

1. Extract the archive to `C:\mingw64\` (or your preferred location)
2. Add `C:\mingw64\bin` to your Windows PATH environment variable

**To add to PATH:**
1. Open Settings → System → About → Advanced system settings
2. Click "Environment Variables"
3. Edit "Path" in System variables
4. Add `C:\mingw64\bin`
5. Click OK and restart cmd/PowerShell

### 3. Install Additional Tools

#### NASM (Assembler)
- Download from: https://www.nasm.us/pub/nasm/releasebuilds/
- Install and add to PATH

#### Python 3
- Download from: https://www.python.org/downloads/
- Check "Add Python to PATH" during installation

#### QEMU (Emulator)
- Download from: https://www.qemu.org/download/#windows
- Add installation directory to PATH

#### xorriso (ISO Creation)
- Download Windows binary or use alternative like `mkisofs`
- Add to PATH

## Verification

Open cmd or PowerShell and test:

```cmd
gcc --version
ld --version
nasm -v
python --version
qemu-system-x86_64 --version
```

## Project Setup

### 1. Extract inDevOS
Extract your project to a development directory (e.g., `C:\Dev\inDevOS`)

### 2. Create Disk Images
```cmd
cd C:\Dev\inDevOS
qemu-img create -f raw sata_disk.img 512M
qemu-img create -f raw nvme_disk.img 1G
```

### 3. Build the OS

**Using PowerShell script (recommended):**
```powershell
.\build_windows.ps1
```

**Using batch file:**
```cmd
build_windows.bat
```

**Manual build:**
```cmd
REM Clean previous build
if exist obj rmdir /s /q obj

REM Build with Lua support
build_windows.bat
```

### 4. Run the OS
```cmd
run_windows.bat
```

## Build Scripts

The following scripts are provided:

- `build_windows.ps1` - PowerShell build script with advanced features
- `build_windows.bat` - Basic batch file for cmd compatibility  
- `run_windows.bat` - QEMU execution script

## Advantages of This Approach

✅ **Native Windows workflow** - Use cmd/PowerShell/Windows Terminal
✅ **GNU toolchain compatibility** - Existing Makefile works with minimal changes
✅ **No Unix layer** - Direct Windows execution
✅ **Smaller footprint** - Only install what you need
✅ **Better IDE integration** - Works with Visual Studio Code, CLion, etc.

## Troubleshooting

### Common Issues

**"gcc not found"**
- Verify MinGW-w64 bin directory is in PATH
- Restart terminal after PATH changes
- Use full path: `C:\mingw64\bin\gcc`

**"ld: cannot find linker.ld"**
- Run from project root directory
- Verify `linker.ld` file exists

**"xorriso not found"**
- Install xorriso or use alternative ISO creation tool
- Build will complete without ISO if xorriso missing

**"Python not found"**
- Install Python 3 and add to PATH
- Or use full path in scripts

### File Paths

Windows uses backslashes, but MinGW tools accept forward slashes:
```cmd
REM Both work:
gcc -I src\include ...
gcc -I src/include ...
```

### Long Path Support

Enable long path support in Windows 10/11:
1. Run `gpedit.msc` as administrator
2. Navigate to: Computer Configuration → Administrative Templates → System → Filesystem
3. Enable "Enable Win32 long paths"

## Comparison with MSYS2

| Feature | MinGW-w64 Standalone | MSYS2 |
|---------|---------------------|-------|
| Installation Size | ~200MB | ~2GB |
| Terminal | Native Windows | Unix-like bash |
| Package Manager | Manual | pacman |
| IDE Integration | Excellent | Good |
| Build Speed | Fast | Fast |
| Compatibility | Windows-native | Unix-like |

## Next Steps

1. **Test the build system**: Run `build_windows.bat` to verify everything works
2. **Customize paths**: Edit scripts if tools are installed in non-standard locations
3. **IDE setup**: Configure your preferred IDE to use MinGW-w64 toolchain
4. **Development**: Start modifying the OS code using your preferred Windows tools

## Getting Help

If you encounter issues:
1. Check PATH environment variable
2. Verify all tools are installed correctly
3. Review build script output for specific errors
4. Ensure you're running from the project root directory

This setup gives you a fully functional inDevOS development environment using native Windows tools while maintaining compatibility with the existing build system.

























