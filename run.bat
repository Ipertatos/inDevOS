@echo off
REM Simple run script for Windows
REM Assumes the project is already built

echo Starting inDevOS in QEMU...
echo.
echo Serial output will be saved to serial.log
echo Press Ctrl+Alt+G to release mouse in QEMU
echo Press Ctrl+C in this window to stop QEMU
echo.

REM Check if OS ISO exists
if not exist os.iso (
    echo Error: os.iso not found!
    echo Please build the project first with: build.bat
    echo Or: make -f Makefile.windows ENABLE_LUA=1
    pause
    exit /b 1
)

REM Run QEMU with Windows-friendly paths
qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 1G -M q35 ^
    -drive file=sata_disk.img,format=raw,index=0 ^
    -drive file=nvme_disk.img,format=raw,index=1,media=disk ^
    -netdev user,id=net0 -device rtl8139,netdev=net0 ^
    -serial file:serial.log

echo QEMU exited.
pause
