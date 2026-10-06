# VMware SVGA-II GPU Driver for inDevOS

This directory contains a complete VMware SVGA-II graphics driver implementation for the inDevOS kernel.

## Overview

The VMware SVGA-II driver provides hardware-accelerated 2D graphics capabilities for virtual machines running under VMware products. It supports:

- PCI device detection and initialization
- Display mode setting (resolution and color depth)
- Hardware-accelerated rectangle fill operations
- Hardware-accelerated rectangle copy operations
- FIFO command queue for efficient graphics operations
- Direct framebuffer access as fallback
- Integration with the inDevOS driver management system

## Files

### Core Driver Implementation
- `include/vmware_svga.h` - Header file with register definitions, constants, and function prototypes
- `src/vmware_svga.c` - Main driver implementation with PCI detection, FIFO management, and graphics operations

### Integration
- `src/driver_adapters.c` - Updated to include VMware SVGA driver registration
- `src/kernel.c` - Updated to initialize VMware SVGA driver during boot

### Testing
- `vmware_svga_test.c` - Comprehensive test suite for driver functionality

## Technical Details

### PCI Device Identification
- Vendor ID: 0x15AD (VMware)
- Device ID: 0x0405 (SVGA-II)
- Class: 0x03 (Display Controller)
- Subclass: 0x00 (VGA Compatible)

### Key Features

#### Hardware Detection
The driver automatically detects VMware SVGA-II devices during kernel initialization by scanning the PCI bus for devices matching the VMware vendor ID and SVGA-II device ID.

#### Register Access
The driver communicates with the hardware through I/O ports:
- Index Port (BAR0 - 1 + 0): Write register index
- Value Port (BAR0 - 1 + 1): Read/write register value

#### FIFO Command Processing
When available, the driver uses the FIFO for hardware-accelerated operations:
- Rectangle fill commands
- Rectangle copy commands
- Screen update commands
- Automatic fallback to direct framebuffer access if FIFO is unavailable

#### Display Mode Management
The driver supports dynamic resolution changes:
- Negotiates supported modes with the virtual hardware
- Validates requested modes against hardware limits
- Updates framebuffer mappings after mode changes

## Driver API

### Core Functions
```c
void vmware_svga_init(void);
bool vmware_svga_is_available(void);
void vmware_svga_get_info(uint32_t *width, uint32_t *height);
int vmware_svga_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
```

### Graphics Operations
```c
void vmware_svga_clear_screen(uint32_t color);
void vmware_svga_fillrect(uint32_t color, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void vmware_svga_rect_copy(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);
```

### Driver Management Integration
```c
int vmware_svga_driver_adapter_init(struct pci_device *dev);
int vmware_svga_driver_adapter_probe(struct pci_device *dev);
void vmware_svga_driver_adapter_status(void);
```

## Usage in Virtual Machines

### VMware Configuration
To use this driver, ensure your VMware virtual machine is configured with:
- VMware SVGA-II graphics adapter (default in most VMware products)
- Sufficient video memory allocation
- Hardware acceleration enabled

### Initialization Sequence
1. PCI bus scan detects VMware SVGA device
2. Driver registers with driver management system
3. I/O ports are mapped and communication established
4. Device capabilities are negotiated
5. Framebuffer and FIFO are mapped to virtual memory
6. Default display mode is set (1024x768x32)
7. Test screen clear operation is performed

## Testing

The included test suite (`vmware_svga_test.c`) provides comprehensive testing:

### Basic Operations Test
- Driver availability check
- Display mode information retrieval
- Screen clearing operations
- Rectangle fill in various colors
- Rectangle copy operations
- Gradient pattern drawing
- Mode switching test

### Performance Test
- Bulk rectangle operations (1000 small rectangles)
- Performance measurement framework (ready for timing integration)

### Running Tests
```c
// From kernel space (integrate into command system)
cmd_vmware_test();

// Or directly call
vmware_svga_test_main();
```

## Integration Notes

### Driver Management System
The VMware SVGA driver is fully integrated with inDevOS's driver management system:
- Automatic device discovery during boot
- Standard driver adapter interface
- Proper initialization and cleanup sequences
- Status reporting capabilities

### Memory Management
- Uses kernel memory allocator for device structures
- Maps hardware memory regions to virtual address space
- Handles both I/O port and memory-mapped I/O access

### Error Handling
- Graceful fallback when hardware is not available
- Bounds checking for all graphics operations
- Validation of mode change requests
- Safe handling of FIFO full conditions

## Future Enhancements

Potential areas for improvement:
- 3D acceleration support (if hardware supports it)
- Hardware cursor implementation
- Multiple display support
- Advanced FIFO commands (patterns, alpha blending)
- Integration with desktop manager for window acceleration
- Performance optimization with interrupt-driven FIFO

## Debugging

### Serial Debug Output
The driver provides extensive debug output through the serial port:
- Device detection and initialization steps
- Register read/write operations
- FIFO command processing
- Error conditions and fallback behavior

### Status Information
Use the status command to get detailed driver information:
```c
vmware_svga_driver_adapter_status();
```

This provides:
- Initialization status
- Current display mode
- Hardware capabilities
- Memory mappings
- FIFO availability

## Compatibility

This driver is specifically designed for:
- VMware SVGA-II virtual graphics adapters
- x86-64 architecture
- inDevOS kernel environment
- PCI-based device discovery

It should work with:
- VMware Workstation
- VMware vSphere
- VMware Fusion
- Other VMware virtualization products using SVGA-II

## Building

The driver is automatically included in the kernel build process through the Makefile's automatic source discovery. No additional build configuration is required.

To build the complete kernel with VMware SVGA support:
```bash
make clean
make
```

The driver will be initialized automatically during kernel boot when running in a VMware virtual machine with SVGA-II graphics.

