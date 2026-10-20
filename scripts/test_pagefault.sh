#!/bin/bash

# Start QEMU in the background
qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -net nic,model=rtl8139 -drive file=sata_disk.img,format=raw,index=0 -drive file=nvme_disk.img,format=raw,index=1,media=disk -serial file:serial.log &

# Wait for the kernel to boot
sleep 10

# Send the mount command that triggers a page fault
echo "Triggering page fault with mount 0 command..."

# For now, just wait and let the user manually trigger it
echo "Please manually run 'mount 0' in the QEMU console to trigger the page fault"
echo "Check serial.log for the enhanced stack trace output"

wait 