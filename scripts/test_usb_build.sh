#!/bin/bash
# Test script for USB-only build

echo "Testing USB-only build..."
echo "========================================="

echo "1. Testing make test-usb (kernel only)..."
if make test-usb; then
    echo "✓ USB-only kernel build successful"
else
    echo "✗ USB-only kernel build failed"
    exit 1
fi

echo ""
echo "2. Testing make test-usb-iso (full ISO)..."
if make test-usb-iso; then
    echo "✓ USB-only ISO build successful"
else
    echo "✗ USB-only ISO build failed"
    exit 1
fi

echo ""
echo "Build test completed successfully!"
echo "========================================="
echo ""
echo "Available USB test commands:"
echo "  make test-usb        - Build USB-only kernel"
echo "  make test-usb-iso    - Build USB-only ISO"
echo "  make test-usb-run    - Build and run with QEMU (USB keyboard & mouse)"
echo ""
echo "To test in QEMU:"
echo "  make test-usb-run"
echo ""
echo "QEMU will automatically configure USB devices:"
echo "  - XHCI USB controller"
echo "  - USB keyboard"
echo "  - USB mouse"
echo ""
echo "PS/2 input will be disabled in this build!"
