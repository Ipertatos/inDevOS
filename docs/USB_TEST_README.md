# USB-Only Input Test Mode

This document describes the USB-only test mode that disables PS/2 input and uses USB for keyboard and mouse.

## Overview

The USB-only test mode is a special build configuration that:
- **Disables PS/2 keyboard and mouse initialization**
- **Uses USB HID devices for all input**
- **Provides the same functionality through USB polling**

This is useful for:
- Testing USB input implementation
- Debugging USB device enumeration
- Supporting systems without PS/2 ports
- Modern hardware compatibility testing

## Build Commands

### Build USB-only kernel:
```bash
make test-usb
```
This builds `kernel_final.bin` with PS/2 disabled.

### Build USB-only ISO:
```bash
make test-usb-iso
```
This builds `os.iso` with PS/2 disabled.

### Build and run with QEMU:
```bash
make test-usb-run
```
This builds the ISO and runs it in QEMU with:
- XHCI USB controller
- USB keyboard device
- USB mouse device

## QEMU Configuration

When using `make test-usb-run`, QEMU is configured with:
```bash
-device qemu-xhci -device usb-kbd -device usb-mouse
```

This creates:
- **qemu-xhci**: USB 3.0 controller
- **usb-kbd**: USB keyboard device
- **usb-mouse**: USB mouse device

## Implementation Details

### Compile-time Flag
The USB-only mode is controlled by the `DISABLE_PS2_INPUT` preprocessor flag.

### Code Changes
When `DISABLE_PS2_INPUT` is defined:
1. **Initialization**: Skips `keyboard_init()` and `mouse_init()`
2. **Main Loop**: Skips PS/2 keyboard echo processing
3. **Command Processing**: Disables PS/2 command buffer processing
4. **USB Focus**: Relies entirely on USB input polling

### USB Input Functions Used
- `usb_keyboard_integrate_with_system()` - Poll USB keyboards
- `usb_mouse_poll_all()` - Poll USB mice
- `usb_keyboard_has_input()` - Check for USB keyboard input
- `usb_keyboard_get_char()` - Get USB keyboard characters

## Testing

Run the automated test:
```bash
./scripts/test_usb_build.sh
```

This will:
1. Test USB-only kernel build
2. Test USB-only ISO build
3. Report success/failure
4. Show usage instructions

## Limitations

In USB-only mode:
- **Command execution**: PS/2 command buffer is disabled
- **Input lag**: USB polling may have different timing than PS/2 interrupts
- **Device discovery**: USB devices must be enumerated before input works
- **Boot delay**: Added 1-second delay for USB device enumeration

## Normal vs USB-only Mode

| Feature | Normal Mode | USB-only Mode |
|---------|-------------|---------------|
| PS/2 Keyboard | ✓ Enabled | ✗ Disabled |
| PS/2 Mouse | ✓ Enabled | ✗ Disabled |
| USB Keyboard | ✓ Available | ✓ Primary |
| USB Mouse | ✓ Available | ✓ Primary |
| Command Input | PS/2 + USB | USB only |
| Boot Time | Normal | +1 sec delay |

## Troubleshooting

### No input detected:
1. Check if USB devices enumerated: Look for "USB:" messages in serial log
2. Verify QEMU USB configuration: Ensure `-device usb-kbd -device usb-mouse`
3. Check enumeration delay: USB devices need time to be discovered

### Build errors:
1. Clean build directory: `make clean`
2. Check compiler flags: Ensure `DISABLE_PS2_INPUT` is defined
3. Verify dependencies: Ensure all USB functions are implemented

## Return to Normal Mode

To return to normal PS/2 + USB mode:
```bash
make clean
make all
```

This removes the `DISABLE_PS2_INPUT` flag and restores normal functionality.
