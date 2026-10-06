# Windows Transfer Checklist

## Files to Transfer

### ✅ Essential Files (Created)
- `inDevOS-backup-*.tar.gz` - Complete source code backup
- `WINDOWS_SETUP.md` - Setup instructions for Windows
- `Makefile.windows` - Windows-specific makefile
- `build.bat` - Windows build script
- `run.bat` - Windows run script
- `PROJECT_STATUS.md` - Current development status

### 📁 Included in Backup
- All source code (`src/`)
- Build configuration (`Makefile`, `linker.ld`)
- Limine bootloader files (`limine/`)
- Lua source code (`third_party/lua-5.4/`)
- Configuration files (`limine.cfg`)
- Python scripts (`symtab_gen.py`)

### ❌ Excluded from Backup
- Build artifacts (`obj/`, `*.o`, `*.bin`, `*.iso`)
- Disk images (`*.img`)
- Log files (`*.log`)
- Git history (`.git/`)

## Transfer Steps

1. **Download the backup**: `inDevOS-backup-YYYYMMDD-HHMM.tar.gz`
2. **Copy Windows-specific files**:
   - `WINDOWS_SETUP.md`
   - `Makefile.windows`
   - `build.bat`
   - `run.bat`
   - `PROJECT_STATUS.md`
   - `TRANSFER_CHECKLIST.md`

## Windows Setup Process

1. **Install MSYS2** from https://www.msys2.org/
2. **Install QEMU** from https://www.qemu.org/download/#windows
3. **Open MSYS2 MINGW64 terminal** and run:
   ```bash
   pacman -Syu
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils make tar python3 mingw-w64-x86_64-xorriso
   ```
4. **Extract backup** to your development directory
5. **Copy Windows-specific files** to the extracted directory
6. **Create disk images**:
   ```bash
   # Create empty disk images for QEMU
   qemu-img create -f raw sata_disk.img 512M
   qemu-img create -f raw nvme_disk.img 1G
   
   # Format them (optional, for file storage)
   # You can copy PNG files to these later
   ```

## First Build on Windows

```batch
REM Method 1: Use the batch file
build.bat

REM Method 2: Use make directly
make -f Makefile.windows ENABLE_LUA=1

REM Method 3: Manual steps
make -f Makefile.windows clean
make -f Makefile.windows ENABLE_LUA=1
```

## Running the OS

```batch
REM Method 1: Use the batch file
run.bat

REM Method 2: Use make
make -f Makefile.windows run

REM Method 3: Direct QEMU command
qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log
```

## Testing the Image System

1. **Copy a PNG file** to one of the disk images (requires additional tools)
2. **Boot the OS** and use the command: `img filename.png`
3. **Check serial.log** for detailed debug output

## Current Image Support Status

- ✅ **Simple images** (SIMG format) - fully working
- ⚠️ **PNG images** - partially working (colorful noise due to incomplete DEFLATE)
- ❌ **Other formats** - not yet implemented

## Development Priorities on Windows

1. **Test the current build system** works properly
2. **Fix any Windows-specific build issues**
3. **Continue PNG DEFLATE implementation** (dynamic Huffman blocks)
4. **Implement PNG filter algorithms**
5. **Add more image format support**

## Notes

- Serial output goes to `serial.log` in the project directory
- Use `tail -f serial.log` in MSYS2 to monitor output in real-time
- QEMU keyboard shortcut: Ctrl+Alt+G to release mouse
- Build time is typically 30-60 seconds on modern hardware

## File Structure After Transfer

```
inDevOS/
├── src/                    # Source code
├── third_party/            # Lua source
├── limine/                 # Bootloader
├── Makefile               # Original Linux makefile
├── Makefile.windows       # Windows-specific makefile
├── build.bat              # Windows build script
├── run.bat                # Windows run script
├── linker.ld              # Memory layout
├── limine.cfg             # Boot configuration
├── symtab_gen.py          # Symbol table generator
├── WINDOWS_SETUP.md       # Setup instructions
├── PROJECT_STATUS.md      # Development status
└── TRANSFER_CHECKLIST.md  # This file
```

Ready for Windows development! 🚀
