@echo off
REM Windows batch file for building inDevOS
REM Requires MSYS2 with development tools installed

echo Building inDevOS on Windows...

REM Check if we're in MSYS2 environment
where gcc >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: GCC not found. Please run this from MSYS2 MINGW64 terminal.
    echo Or ensure MSYS2 tools are in your PATH.
    pause
    exit /b 1
)

REM Clean previous build
echo Cleaning previous build...
if exist obj rmdir /s /q obj
if exist boot.bin del boot.bin
if exist kernel_final.bin del kernel_final.bin
if exist os.iso del os.iso
if exist kernel.sym del kernel.sym
if exist src\kernel_symtab.c del src\kernel_symtab.c
if exist iso rmdir /s /q iso

REM Build with Lua support
echo Building kernel with Lua support...
make -f Makefile.windows ENABLE_LUA=1

if %errorlevel% neq 0 (
    echo Build failed!
    pause
    exit /b 1
)

echo Build completed successfully!
echo.
echo To run the OS:
echo   make -f Makefile.windows run
echo.
echo Or manually:
echo   qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log
echo.
pause
