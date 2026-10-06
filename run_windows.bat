@echo off
REM Windows run script for inDevOS
REM Requires QEMU to be installed and in PATH

setlocal

set QEMU=qemu-system-x86_64
set ISO_FILE=os.iso
set MEMORY=1G
set CPUS=4

REM Check if ISO exists
if not exist "%ISO_FILE%" (
    echo [ERROR] OS image not found: %ISO_FILE%
    echo.
    echo Build the OS first:
    echo   build_windows.bat
    echo.
    pause
    exit /b 1
)

REM Check if QEMU is available
where %QEMU% >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] QEMU not found in PATH
    echo.
    echo Please install QEMU and add it to your PATH:
    echo   https://www.qemu.org/download/#windows
    echo.
    echo Default installation path:
    echo   C:\Program Files\qemu\qemu-system-x86_64.exe
    echo.
    pause
    exit /b 1
)

REM Create disk images if they don't exist
if not exist sata_disk.img (
    echo [INFO] Creating SATA disk image...
    qemu-img create -f raw sata_disk.img 512M
)

if not exist nvme_disk.img (
    echo [INFO] Creating NVMe disk image...
    qemu-img create -f raw nvme_disk.img 1G
)

echo.
echo ===============================
echo Starting inDevOS in QEMU
echo ===============================
echo.
echo ISO: %ISO_FILE%
echo Memory: %MEMORY%
echo CPUs: %CPUS%
echo Serial output: serial.log
echo.
echo Press Ctrl+Alt+G to release mouse from QEMU
echo Press Ctrl+Alt+Q to quit QEMU
echo.

REM Start QEMU
%QEMU% ^
    -boot d ^
    -cdrom %ISO_FILE% ^
    -smp %CPUS% ^
    -m %MEMORY% ^
    -M q35 ^
    -drive file=sata_disk.img,format=raw,index=0 ^
    -drive file=nvme_disk.img,format=raw,index=1,media=disk ^
    -netdev user,id=net0 -device rtl8139,netdev=net0 ^
    -serial file:serial.log

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] QEMU failed to start
    echo.
    echo Check that:
    echo   1. QEMU is properly installed
    echo   2. %ISO_FILE% exists and is valid
    echo   3. No other instances of QEMU are running
    echo.
    pause
    exit /b 1
)

echo.
echo [INFO] QEMU session ended
echo.
echo Serial output saved to: serial.log
echo.
pause

























