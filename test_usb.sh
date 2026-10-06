#!/bin/bash

echo "=== inDevOS USB Driver Test ==="
echo "This script will test the USB driver functionality"
echo ""

# Build the OS
echo "Building inDevOS with USB support..."
make clean > /dev/null 2>&1
make os.iso > /dev/null 2>&1

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "Build successful!"
echo ""

# Run the OS with USB support
echo "Starting inDevOS with USB emulation..."
echo "The OS will boot and you can test USB commands:"
echo "  - Type 'usb' to see USB status"
echo "  - Type 'usbenum' to enumerate USB devices"
echo "  - Type 'help' to see all available commands"
echo ""

# Run QEMU with USB support
qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 4G -M q35 \
    -audiodev pa,id=speaker -machine pcspk-audiodev=speaker \
    -net user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -serial file:serial.log \
    -device pcie-root-port,id=pcie.1,slot=1 \
    -device pcie-root-port,id=pcie.2,slot=2 \
    -device pcie-root-port,id=pcie.3,slot=3 \
    -device virtio-gpu-pci,id=gpu.0,bus=pcie.1,max_outputs=1 \
    -device secondary-vga,id=gpu.1,bus=pcie.2 \
    -vga virtio \
    -usb -device usb-tablet

echo ""
echo "OS session ended. Check serial.log for detailed USB initialization logs." 