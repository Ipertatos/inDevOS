# inDevOS USB Driver

This document describes the USB driver implementation in inDevOS.

## Overview

The USB driver provides basic USB controller detection and device enumeration capabilities. It's designed as a foundation that can be extended to support specific USB device classes like mass storage, HID devices, and more.

## Features

### Current Implementation
- **USB Controller Detection**: Automatically detects USB controllers via PCI enumeration
- **Controller Types Supported**: UHCI, OHCI, EHCI, XHCI (detection only)
- **Device Enumeration**: Basic device discovery and descriptor reading
- **Command Line Interface**: USB status and enumeration commands
- **Debug Logging**: Comprehensive serial debug output

### Supported USB Controllers
- **UHCI** (Universal Host Controller Interface) - USB 1.1
- **OHCI** (Open Host Controller Interface) - USB 1.1
- **EHCI** (Enhanced Host Controller Interface) - USB 2.0
- **XHCI** (eXtensible Host Controller Interface) - USB 3.0

## Architecture

### Core Components

1. **USB Controller Management** (`usb_controller_t`)
   - PCI device association
   - Base address mapping
   - Controller type identification
   - Device list management

2. **USB Device Management** (`usb_device_t`)
   - Device addressing
   - Descriptor storage
   - Class identification
   - Controller association

3. **USB Protocol Support**
   - Setup packet handling
   - Device descriptor reading
   - Address assignment
   - Configuration descriptor reading

### File Structure
```
src/
├── usb.h          # USB driver header with structures and declarations
├── usb.c          # USB driver implementation
└── cmd.c          # Command line interface integration
```

## Usage

### Commands Available

1. **`usb`** - Show USB status
   - Displays controller information
   - Lists detected devices
   - Shows controller types and addresses

2. **`usbenum`** - Enumerate USB devices
   - Re-runs device enumeration
   - Updates device count
   - Useful for hot-plug scenarios

3. **`help`** - Show all available commands including USB commands

### Example Output

```
USB Status:
USB controllers detected: 1

Controller 0:
  Type: 0x00 (UHCI)
  Base Address: 0xffffffff80000000
  PCI Device: Bus 0, Device 31, Function 3
  Devices: 1
    Device 0: addr=1, vid=0x1234, pid=0x5678, class=0x08

Total USB devices: 1
USB driver status: Basic infrastructure ready
```

## Implementation Details

### Controller Detection
The driver scans PCI devices for USB controllers (class 0x0C, subclass 0x05) and initializes them based on their programming interface:

- `0x00` - UHCI
- `0x10` - OHCI  
- `0x20` - EHCI
- `0x30` - XHCI

### Device Enumeration
1. **Address Assignment**: Devices are assigned addresses starting from 1
2. **Descriptor Reading**: Device descriptors are read to identify device class
3. **Configuration**: Configuration descriptors are read (placeholder)
4. **Class Detection**: Device class is determined for future driver binding

### Memory Management
- Controllers are stored in a static array (max 4 controllers)
- Devices are allocated dynamically and linked to controllers
- All memory is managed through the kernel's memory allocator

## Future Enhancements

### Planned Features
1. **USB Mass Storage Support**
   - SCSI command translation
   - Bulk transfer implementation
   - Storage device integration

2. **USB HID Support**
   - Keyboard and mouse support
   - Report descriptor parsing
   - Input event handling

3. **USB Hub Support**
   - Multi-tier device enumeration
   - Port status monitoring
   - Hot-plug detection

4. **Enhanced Protocol Support**
   - Full USB transaction handling
   - Interrupt transfer support
   - Isochronous transfer support

### Controller-Specific Implementations
- **UHCI**: Frame list and transfer descriptor management
- **OHCI**: Endpoint descriptor and transfer list handling
- **EHCI**: Queue head and transfer descriptor structures
- **XHCI**: Command and transfer ring management

## Testing

### Test Environment
The USB driver is tested using QEMU with USB emulation:

```bash
# Run the test script
./test_usb.sh

# Or manually with QEMU
qemu-system-x86_64 -boot d -cdrom os.iso -usb -device usb-tablet
```

### Debug Output
USB initialization and operation is logged to the serial console. Check `serial.log` for detailed debug information.

## Integration

### Kernel Integration
The USB driver is automatically initialized during kernel boot:

```c
// In kernel.c
printf("Initializing USB subsystem\n");
usb_init();
```

### Storage Integration
USB mass storage devices can be integrated with the existing storage subsystem by implementing the storage operations interface.

## Development Notes

### Current Limitations
1. **Placeholder Implementations**: Many functions return dummy data
2. **No Actual Transfers**: Setup packets are logged but not sent
3. **Limited Device Support**: Only basic enumeration is implemented
4. **No Interrupt Handling**: USB interrupts are not yet handled

### Debugging
- Use `DEBUG_SERIAL()` for detailed logging
- Check PCI device detection in `serial.log`
- Monitor USB command output for status information

## Contributing

When extending the USB driver:

1. **Follow the existing pattern** for controller and device management
2. **Add debug logging** for new functionality
3. **Update command interface** for new features
4. **Test with QEMU** before implementing hardware-specific code
5. **Document new features** in this README

## References

- USB 2.0 Specification
- UHCI Design Guide
- OHCI Specification
- EHCI Specification
- XHCI Specification 