#!/bin/bash

# Build syscall test program
echo "Building syscall test program..."

# Use the same compiler as the kernel
if [ -f ~/opt/cross/bin/x86_64-elf-gcc ]; then
    CC=~/opt/cross/bin/x86_64-elf-gcc
else
    CC=gcc
fi

# Compile the test program (freestanding, no standard library)
$CC -g -m64 -ffreestanding -fno-pie -mcmodel=kernel -nostdlib -nodefaultlibs -I. -o sc.exe sc.c

if [ $? -eq 0 ]; then
    echo "Syscall test program built successfully!"
    echo "Copy it to your disk image and run with: exec sc.exe"
    ./putfilesinsata.sh sc.exe
else
    echo "Build failed!"
fi
