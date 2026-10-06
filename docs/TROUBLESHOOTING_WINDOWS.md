# Windows Development Troubleshooting Guide

## Common Issues and Solutions

### Build Issues

#### "gcc not found" or "command not found"

**MinGW-w64 Setup:**
```cmd
REM Check if GCC is in PATH
where gcc

REM If not found, add MinGW-w64 to PATH:
REM Add C:\mingw64\bin to Windows PATH environment variable
```

**MSYS2 Setup:**
```bash
# Run from MSYS2 MINGW64 terminal, not regular MSYS2
# Install GCC if missing:
pacman -S mingw-w64-x86_64-gcc
```

#### "ld: cannot find linker.ld"

**Solution:**
- Ensure you're running build commands from the project root directory
- Verify `linker.ld` file exists in the project root

#### "nasm: command not found"

**MinGW-w64:**
- Download NASM from https://www.nasm.us/
- Add NASM installation directory to PATH

**MSYS2:**
```bash
pacman -S nasm
```

#### "python: command not found"

**Windows:**
- Install Python 3 from https://www.python.org/
- Check "Add Python to PATH" during installation

**MSYS2:**
```bash
pacman -S python3
```

#### "xorriso not found" - ISO creation fails

**Impact:** Kernel builds successfully, but no bootable ISO created

**Solutions:**
1. **Install xorriso** (recommended):
   - Download GNU xorriso for Windows
   - Add to PATH

2. **Use alternative ISO tools:**
   ```cmd
   REM Use mkisofs instead
   mkisofs -b boot/limine/limine-bios-cd.bin ...
   ```

3. **Skip ISO creation:**
   - Use `kernel_final.bin` directly with QEMU
   - Boot from kernel file instead of ISO

### Runtime Issues

#### QEMU fails to start

**Check QEMU installation:**
```cmd
qemu-system-x86_64 --version
```

**Common solutions:**
- Install QEMU from https://www.qemu.org/download/#windows
- Add QEMU to Windows PATH
- Use full path in scripts if needed

#### "Permission denied" errors

**Windows-specific:**
- Run as Administrator if needed
- Check Windows Defender / antivirus interference
- Verify file permissions on project directory

#### Serial output not appearing

**Check serial.log:**
```cmd
REM Monitor serial output in real-time
type serial.log
REM Or use PowerShell
Get-Content serial.log -Wait
```

### Environment Issues

#### PATH not working after installation

**Solutions:**
1. **Restart terminal** after PATH changes
2. **Restart Windows** if needed
3. **Check PATH manually:**
   ```cmd
   echo %PATH%
   ```
4. **Use full paths temporarily:**
   ```cmd
   C:\mingw64\bin\gcc --version
   ```

#### Long path issues on Windows

**Enable long path support:**
1. Run `gpedit.msc` as Administrator
2. Navigate to: Computer Configuration → Administrative Templates → System → Filesystem  
3. Enable "Enable Win32 long paths"

**Alternative:** Use short path names or move project closer to drive root

#### Anti-virus interference

**Symptoms:**
- Random build failures
- "Access denied" errors
- Slow compilation

**Solutions:**
1. Add project directory to antivirus exclusions
2. Add MinGW-w64/MSYS2 directories to exclusions
3. Temporarily disable real-time protection during builds

### Performance Issues

#### Slow compilation

**Solutions:**
1. **Use faster storage** (SSD recommended)
2. **Add antivirus exclusions** for build directories
3. **Parallel compilation:**
   ```cmd
   REM Enable parallel builds (adjust number for your CPU cores)
   set MAKEFLAGS=-j4
   ```

#### QEMU performance issues

**Solutions:**
1. **Enable Windows Hypervisor Platform:**
   - Windows Features → Hyper-V → Windows Hypervisor Platform
2. **Use hardware acceleration:**
   ```cmd
   qemu-system-x86_64 -accel whpx ...
   ```
3. **Allocate more memory:**
   ```cmd
   qemu-system-x86_64 -m 2G ...
   ```

### IDE Integration Issues

#### Visual Studio Code

**Configure for MinGW-w64:**
```json
// .vscode/c_cpp_properties.json
{
    "configurations": [
        {
            "name": "MinGW64",
            "includePath": [
                "${workspaceFolder}/**",
                "C:/mingw64/include/**"
            ],
            "compilerPath": "C:/mingw64/bin/gcc.exe",
            "cStandard": "c17",
            "intelliSenseMode": "gcc-x64"
        }
    ]
}
```

**Build task:**
```json
// .vscode/tasks.json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build inDevOS",
            "type": "shell",
            "command": "./build_windows.bat",
            "group": {
                "kind": "build",
                "isDefault": true
            }
        }
    ]
}
```

#### CLion

**Configure toolchain:**
1. File → Settings → Build, Execution, Deployment → Toolchains
2. Add MinGW-w64 toolchain
3. Set GCC path: `C:\mingw64\bin\gcc.exe`
4. Set G++ path: `C:\mingw64\bin\g++.exe`

### Network Issues (for OS testing)

#### TAP interface setup

**Windows doesn't have native TAP:**
- Use OpenVPN TAP driver
- Or use QEMU user mode networking:
  ```cmd
  qemu-system-x86_64 -netdev user,id=net0 ...
  ```

### Debugging

#### Enable verbose output

**PowerShell script:**
```powershell
.\build_windows.ps1 -Verbose
```

**Batch script:**
```cmd
REM Edit build_windows.bat and add:
set DEBUG=1
```

#### GDB debugging

**Install GDB:**
```cmd
REM MinGW-w64 includes GDB
gdb kernel_final.bin
```

**QEMU with GDB:**
```cmd
REM Terminal 1: Start QEMU with GDB stub
qemu-system-x86_64 -s -S -cdrom os.iso ...

REM Terminal 2: Connect GDB
gdb kernel_final.bin
(gdb) target remote localhost:1234
(gdb) continue
```

### Getting Help

If you're still stuck:

1. **Check project status**: Review `PROJECT_STATUS.md` for known issues
2. **Verify setup**: Follow setup guides step by step
3. **Search for error messages**: Look up specific error text
4. **Create minimal test**: Try building a simple C program first
5. **Check versions**: Ensure all tools are recent versions

### Useful Commands

**Environment verification:**
```cmd
REM Check all tools
gcc --version
ld --version  
nasm -v
python --version
qemu-system-x86_64 --version

REM Check PATH
echo %PATH%

REM Check current directory
cd
dir
```

**Clean rebuild:**
```cmd
REM Full clean and rebuild
build_windows.bat clean
build_windows.bat
```

**Test QEMU:**
```cmd
REM Quick QEMU test (should show QEMU monitor)
qemu-system-x86_64 -monitor stdio
(qemu) quit
```

























