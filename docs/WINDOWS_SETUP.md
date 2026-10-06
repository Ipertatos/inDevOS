# inDevOS Windows Development Setup

## Choose Your Development Environment

You have **two options** for setting up inDevOS development on Windows:

### Option 1: Native Windows Tools (Recommended)
- Use **cmd/PowerShell/Windows Terminal** 
- MinGW-w64 standalone for GNU toolchain
- Better IDE integration and Windows workflow
- **See: `MINGW_SETUP.md` for complete instructions**

### Option 2: MSYS2 Environment
- Unix-like environment on Windows
- Full package management with pacman
- Continue reading this guide

---

## Option 2: MSYS2 Setup

### 1. Install Required Tools

1. **MSYS2** (https://www.msys2.org/)
   - Provides GCC, Make, and Unix-like tools for Windows
   - Install to default location (C:\msys64)

2. **QEMU** (https://www.qemu.org/download/#windows)
   - For running and testing the OS
   - Add to system PATH

3. **Git for Windows** (if not already installed)

### 2. MSYS2 Setup

Open MSYS2 MINGW64 terminal and install packages:

```bash
# Update package database
pacman -Syu

# Install development tools
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-binutils
pacman -S make
pacman -S tar
pacman -S python3

# Install additional tools
pacman -S mingw-w64-x86_64-xorriso
```

### 3. Project Setup

1. Extract the backup archive to your preferred location (e.g., `C:\Dev\inDevOS`)
2. Open MSYS2 MINGW64 terminal
3. Navigate to your project directory:
   ```bash
   cd /c/Dev/inDevOS  # Adjust path as needed
   ```

### 4. Windows-Specific Modifications

The Makefile may need some adjustments for Windows. Key areas:

- **QEMU path**: May need full path if not in system PATH
- **Serial output**: Windows uses COM ports differently
- **File permissions**: `sudo` not available in MSYS2

## Current Project Status

### Completed Features
- ✅ Kernel boots successfully with BSS clearing fix
- ✅ Lua Desktop Environment (LuaDE) integration
- ✅ FAT32 filesystem support
- ✅ Basic PNG decoder with DEFLATE decompression
- ✅ `img` command for loading and displaying images
- ✅ Simple image format support
- ✅ Framebuffer graphics output

### PNG Decoder Status
- ✅ PNG signature validation and chunk parsing
- ✅ DEFLATE uncompressed blocks (type 0)
- ✅ DEFLATE fixed Huffman blocks (type 1) with LZ77
- ⚠️  DEFLATE dynamic Huffman blocks (type 2) - partial implementation
- ⚠️  PNG filters - basic implementation (only NONE filter fully supported)
- ✅ RGB to RGBA pixel conversion

### Known Issues
1. Dynamic Huffman DEFLATE blocks need complete implementation
2. PNG filter types (SUB, UP, AVERAGE, PAETH) need proper implementation
3. Some PNGs display as colorful noise due to incomplete decompression

## Building on Windows

```bash
# Clean build
make clean

# Build with Lua support
make ENABLE_LUA=1

# Run in QEMU
make run
```

## Testing Images

The system currently supports:
- Simple uncompressed images (SIMG format)
- PNG files (with limitations on compression types)

Test with: `img filename.png`

## Next Development Steps

1. Complete DEFLATE dynamic Huffman implementation
2. Implement remaining PNG filter types
3. Add support for other image formats (JPEG, BMP)
4. Improve error handling and memory management

## Troubleshooting

### Common Windows Issues

1. **Path separators**: Use forward slashes in MSYS2
2. **File permissions**: No sudo needed, files may need to be writable
3. **QEMU not found**: Add QEMU installation directory to Windows PATH
4. **Build errors**: Ensure all MSYS2 packages are installed correctly

### Serial Output
On Windows, serial output goes to `serial.log` in the project directory.
Monitor with: `tail -f serial.log` (in MSYS2) or use a text editor that auto-refreshes.

## File Structure

```
inDevOS/
├── src/                 # Source code
│   ├── kernel.c        # Main kernel entry
│   ├── cmd.c           # Command interpreter with img command
│   ├── png.c           # PNG decoder implementation
│   ├── png.h           # PNG decoder headers
│   ├── simple_img.c    # Simple image format decoder
│   ├── simple_img.h    # Simple image format headers
│   └── ...             # Other kernel modules
├── limine/             # Bootloader files
├── Makefile            # Build configuration
├── linker.ld           # Kernel memory layout
└── WINDOWS_SETUP.md    # This file
```

## Alternative: Native Windows Setup

For a **native Windows development experience** without MSYS2:

1. **See `MINGW_SETUP.md`** - Complete guide for MinGW-w64 standalone
2. **See `WINDOWS_NATIVE_SETUP.md`** - Advanced Windows-native options
3. **Use provided scripts**: 
   - `build_windows.ps1` (PowerShell)
   - `build_windows.bat` (Batch)
   - `run_windows.bat` (QEMU execution)

## Contact/Support

The PNG decoder is the most complex part of the current implementation. 
The DEFLATE algorithm with dynamic Huffman coding is quite intricate.
Consider looking into existing libraries or reference implementations
for guidance on the mathematical aspects of Huffman tree construction.

## Summary of Windows Options

| Approach | Tools | Terminal | Complexity | Recommended For |
|----------|-------|----------|------------|-----------------|
| **MinGW-w64** | Native Windows | cmd/PowerShell | Low | Most users |
| **MSYS2** | Unix-like | bash | Medium | Unix familiarity |
| **Full Native** | MSVC/Clang | cmd/PowerShell | High | Advanced users |

**Start with MinGW-w64 setup** (`MINGW_SETUP.md`) for the best Windows experience.
