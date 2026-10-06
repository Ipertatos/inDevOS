Write-Host "Starting inDevOS with Integrated Graphics Passthrough..." -ForegroundColor Green
Write-Host ""

# Build the OS first
Write-Host "Building inDevOS..." -ForegroundColor Yellow
& make os.iso

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed! Please check the errors above." -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "Launching QEMU with integrated graphics support..." -ForegroundColor Yellow

# Run QEMU with integrated graphics configuration for Windows
& qemu-system-x86_64.exe `
    -boot d -cdrom os.iso `
    -smp 4 -m 4G -M q35 `
    -audiodev dsound,id=speaker `
    -machine pcspk-audiodev=speaker `
    -netdev user,id=net0 -device rtl8139,netdev=net0 `
    -drive file=sata_disk.img,format=raw,index=0 `
    -drive file=nvme_disk.img,format=raw,index=1,media=disk `
    -serial file:serial.log `
    -device pcie-root-port,id=pcie.1,slot=1 `
    -device pcie-root-port,id=pcie.2,slot=2 `
    -device pcie-root-port,id=pcie.3,slot=3 `
    -device pcie-root-port,id=pcie.4,slot=4 `
    -device virtio-gpu-pci,id=igpu.0,bus=pcie.1,max_outputs=2 `
    -device intel-hda,id=sound0,bus=pcie.2 `
    -device hda-duplex,id=sound0-codec0,bus=sound0.0,cad=0,audiodev=speaker `
    -device secondary-vga,id=igpu.1,bus=pcie.3 `
    -vga virtio `
    -display gtk,gl=on,grab-on-hover=on `
    -accel tcg `
    -cpu qemu64,-ht,-cmp-legacy `
    -machine kernel_irqchip=off `
    -rtc base=localtime `
    -usb -device usb-tablet

Write-Host ""
Write-Host "inDevOS with Integrated Graphics has been launched!" -ForegroundColor Green
Write-Host "Available commands in your OS:" -ForegroundColor Cyan
Write-Host "  - 'gpu'  : Test graphics acceleration and framebuffer" -ForegroundColor White
Write-Host "  - 'gpuz' : Display detailed GPU information" -ForegroundColor White
Write-Host "  - 'help' : Show all available commands" -ForegroundColor White

Read-Host "Press Enter to continue" 