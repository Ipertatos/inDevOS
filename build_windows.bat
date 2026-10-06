@echo off
REM Batch Build Script for inDevOS on Windows
REM Requires: MinGW-w64, NASM, Python 3, QEMU

setlocal enabledelayedexpansion

REM Configuration
set PROJECT_ROOT=%~dp0
set ENABLE_LUA=1

REM Tool paths - adjust if needed
set GCC=gcc
set LD=ld
set NASM=nasm
set PYTHON=python
set QEMU=qemu-system-x86_64

REM Build flags
set CFLAGS=-g -m64 -ffreestanding -fno-pie -mgeneral-regs-only -mcmodel=kernel -fcommon -DDEBUG_PRINT -Iflanterm -Iflanterm/backends -msse2
set LDFLAGS=-T linker.ld -melf_x86_64
set ASFLAGS=-g -F dwarf -f elf64

if "%ENABLE_LUA%"=="1" (
    set CFLAGS=!CFLAGS! -DENABLE_LUA -Ithird_party/lua-5.4/src -DLUA_FLOAT_TYPE=LUA_FLOAT_DOUBLE
)

echo.
echo ===============================
echo inDevOS Windows Build Script
echo ===============================
echo.

REM Check for help flag
if /i "%1"=="help" goto :show_help
if /i "%1"=="/?" goto :show_help
if /i "%1"=="-h" goto :show_help

REM Check for clean flag
if /i "%1"=="clean" goto :clean_build

REM Check dependencies
echo [INFO] Checking dependencies...

where !GCC! >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] GCC not found. Please install MinGW-w64.
    goto :error_exit
)

where !LD! >nul 2>&1  
if %errorlevel% neq 0 (
    echo [ERROR] GNU LD not found. Please install binutils from MinGW-w64.
    goto :error_exit
)

where !NASM! >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] NASM not found. Please install NASM assembler.
    goto :error_exit
)

where !PYTHON! >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Python not found. Please install Python 3.
    goto :error_exit
)

where !QEMU! >nul 2>&1
if %errorlevel% neq 0 (
    echo [WARN] QEMU not found. Install QEMU to run the OS.
)

echo [INFO] Dependencies check passed
echo.

REM Create directories
if not exist obj mkdir obj
if not exist obj\lua mkdir obj\lua

REM Check for Lua sources
if "%ENABLE_LUA%"=="1" (
    if not exist third_party\lua-5.4\src\lapi.c (
        echo [INFO] Fetching Lua sources...
        if exist scripts\fetch_lua.sh (
            bash scripts\fetch_lua.sh
        ) else (
            echo [ERROR] Lua sources not found and fetch script missing
            goto :error_exit
        )
    )
)

echo [INFO] Building kernel objects...

REM Find and compile C sources
set OBJ_FILES=
for /r src %%f in (*.c) do (
    set src_file=%%f
    set rel_path=!src_file:%PROJECT_ROOT%src\=!
    set obj_file=obj\!rel_path:.c=.o!
    set obj_file=!obj_file:\=/!
    
    REM Skip kernel_symtab.c (generated later)
    echo !src_file! | findstr "kernel_symtab.c" >nul
    if !errorlevel! neq 0 (
        REM Create directory for object file
        for %%d in ("!obj_file!") do (
            if not exist "%%~dpd" mkdir "%%~dpd" 2>nul
        )
        
        echo Compiling %%~nxf...
        !GCC! !CFLAGS! -c -o "!obj_file!" "!src_file!"
        if !errorlevel! neq 0 (
            echo [ERROR] Failed to compile !src_file!
            goto :error_exit
        )
        set OBJ_FILES=!OBJ_FILES! "!obj_file!"
    )
)

REM Find and compile ASM sources
for /r src %%f in (*.asm) do (
    set src_file=%%f
    set rel_path=!src_file:%PROJECT_ROOT%src\=!
    set obj_file=obj\!rel_path:.asm=.o!
    set obj_file=!obj_file:\=/!
    
    REM Create directory for object file
    for %%d in ("!obj_file!") do (
        if not exist "%%~dpd" mkdir "%%~dpd" 2>nul
    )
    
    echo Assembling %%~nxf...
    !NASM! !ASFLAGS! -o "!obj_file!" "!src_file!"
    if !errorlevel! neq 0 (
        echo [ERROR] Failed to assemble !src_file!
        goto :error_exit
    )
    set OBJ_FILES=!OBJ_FILES! "!obj_file!"
)

REM Compile Lua sources if enabled
if "%ENABLE_LUA%"=="1" (
    echo [INFO] Building Lua objects...
    set LUA_FILES=lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c lauxlib.c lstrlib.c ltablib.c lmathlib.c
    
    for %%f in (!LUA_FILES!) do (
        set src_file=third_party\lua-5.4\src\%%f
        set obj_file=obj\lua\%%~nf.o
        
        echo Compiling %%f...
        REM Lua needs SSE2, remove -mgeneral-regs-only
        set LUA_CFLAGS=!CFLAGS!
        set LUA_CFLAGS=!LUA_CFLAGS:-mgeneral-regs-only=!
        !GCC! !LUA_CFLAGS! -c -o "!obj_file!" "!src_file!"
        if !errorlevel! neq 0 (
            echo [ERROR] Failed to compile !src_file!
            goto :error_exit
        )
        set OBJ_FILES=!OBJ_FILES! "!obj_file!"
    )
)

echo [INFO] Linking boot.bin...
!LD! !LDFLAGS! -o boot.bin !OBJ_FILES!
if %errorlevel% neq 0 (
    echo [ERROR] Failed to link boot.bin
    goto :error_exit
)

echo [INFO] Generating symbol table...
nm -n boot.bin | findstr " [tT] " > kernel.sym

echo [INFO] Generating symbol table source...
!PYTHON! symtab_gen.py < kernel.sym > src\kernel_symtab.c

echo [INFO] Compiling symbol table...
!GCC! !CFLAGS! -c -o obj\kernel_symtab.o src\kernel_symtab.c
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile kernel_symtab.c
    goto :error_exit
)

echo [INFO] Linking final kernel...
!LD! !LDFLAGS! -o kernel_final.bin !OBJ_FILES! obj\kernel_symtab.o
if %errorlevel% neq 0 (
    echo [ERROR] Failed to link kernel_final.bin
    goto :error_exit
)

echo [INFO] Creating ISO structure...
if not exist iso\boot\limine mkdir iso\boot\limine
if not exist iso\EFI\BOOT mkdir iso\EFI\BOOT

copy kernel_final.bin iso\boot\boot.bin
copy limine.cfg iso\boot\limine\
copy limine\limine-bios.sys iso\boot\limine\
copy limine\limine-bios-cd.bin iso\boot\limine\
copy limine\limine-uefi-cd.bin iso\boot\limine\
copy limine\BOOTX64.EFI iso\EFI\BOOT\
copy limine\BOOTIA32.EFI iso\EFI\BOOT\

echo [INFO] Creating ISO image...
where xorriso >nul 2>&1
if %errorlevel% neq 0 (
    echo [WARN] xorriso not found. Skipping ISO creation.
    echo [WARN] Install xorriso to create bootable ISO.
    goto :build_complete
)

xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label iso -o os.iso
if %errorlevel% neq 0 (
    echo [ERROR] Failed to create ISO
    goto :error_exit
)

echo [INFO] Installing Limine bootloader...
limine\limine bios-install os.iso

:build_complete
echo.
echo [SUCCESS] Build completed successfully!
echo.
echo To run the OS:
echo   run_windows.bat
echo.
echo Or manually:
echo   qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log
echo.
goto :end

:clean_build
echo [INFO] Cleaning build artifacts...
if exist obj rmdir /s /q obj
if exist boot.bin del boot.bin
if exist kernel_final.bin del kernel_final.bin  
if exist os.iso del os.iso
if exist kernel.sym del kernel.sym
if exist src\kernel_symtab.c del src\kernel_symtab.c
if exist iso rmdir /s /q iso
if exist third_party\lua-5.4 rmdir /s /q third_party\lua-5.4
echo [INFO] Clean completed.
goto :end

:show_help
echo.
echo inDevOS Windows Build Script
echo.
echo Usage: build_windows.bat [options]
echo.
echo Options:
echo   clean     Clean build artifacts
echo   help      Show this help message
echo.
echo Examples:
echo   build_windows.bat           # Build with Lua
echo   build_windows.bat clean     # Clean build artifacts
echo.
echo Requirements:
echo   - MinGW-w64 (provides GCC and GNU LD)
echo   - NASM assembler
echo   - Python 3
echo   - QEMU (for running the OS)
echo   - xorriso (for ISO creation)
echo.
goto :end

:error_exit
echo.
echo [ERROR] Build failed!
echo.
echo Make sure you have installed:
echo   - MinGW-w64 (https://www.mingw-w64.org/)
echo   - NASM (https://www.nasm.us/)  
echo   - Python 3 (https://www.python.org/)
echo   - QEMU (https://www.qemu.org/)
echo.
pause
exit /b 1

:end
if not "%1"=="clean" pause

























