# Debug System Usage Guide

## Overview

The kernel now has a centralized debug configuration system in `src/debug_config.h` that allows you to toggle debug output for different subsystems without recompiling individual files.

## Quick Start

### 1. Enable/Disable Debug Output

Edit `src/debug_config.h` and set subsystems to 1 (enabled) or 0 (disabled):

```c
#define DEBUG_RTL8139           1   // Enable RTL8139 debug output
#define DEBUG_NET_IP            0   // Disable IP layer debug output
```

### 2. Using Debug Macros in Code

Include the header:
```c
#include "debug_config.h"
```

Replace regular printf/serial_printf with debug macros:

**Before:**
```c
printf("RTL8139: Received packet of %d bytes\n", length);
serial_printf("RTL8139: TX buffer %d status=0x%08x\n", idx, status);
```

**After:**
```c
DEBUG_PRINT(RTL8139, "RTL8139: Received packet of %d bytes\n", length);
SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: TX buffer %d status=0x%08x\n", idx, status);
```

### 3. Quick Enable/Disable All Network or Storage Debug

```c
// In debug_config.h
#define DEBUG_ALL_NETWORK       1   // Enable all network subsystem debug
#define DEBUG_ALL_STORAGE       1   // Enable all storage subsystem debug
```

## Available Debug Subsystems

### Network
- `DEBUG_RTL8139` - RTL8139 NIC driver
- `DEBUG_NET_IP` - IP layer
- `DEBUG_NET_ICMP` - ICMP protocol
- `DEBUG_NET_ARP` - ARP protocol
- `DEBUG_NET_TCP` - TCP protocol
- `DEBUG_NET_UDP` - UDP protocol
- `DEBUG_NET_DHCP` - DHCP client
- `DEBUG_NET_DNS` - DNS resolver
- `DEBUG_NET_ETHERNET` - Ethernet frames
- `DEBUG_NET_DEVICE` - Network device abstraction

### Storage
- `DEBUG_AHCI` - AHCI/SATA driver
- `DEBUG_IDE` - IDE driver
- `DEBUG_NVME` - NVMe driver
- `DEBUG_FAT32` - FAT32 filesystem
- `DEBUG_STORAGE` - Storage layer

### Hardware Drivers
- `DEBUG_PCI` - PCI bus
- `DEBUG_USB` - USB subsystem
- `DEBUG_KEYBOARD` - Keyboard driver
- `DEBUG_MOUSE` - Mouse driver
- `DEBUG_VGA` - VGA driver
- `DEBUG_VMWARE_SVGA` - VMware SVGA driver
- `DEBUG_AMD_GPU` - AMD GPU driver

### System Components
- `DEBUG_MEMORY` - Memory allocator
- `DEBUG_SCHEDULER` - Process/thread scheduler
- `DEBUG_TASK_MANAGER` - Cooperative task manager
- `DEBUG_SYSCALL` - System calls
- `DEBUG_INTERRUPTS` - Interrupt handling
- `DEBUG_TIMER` - Timer/HPET

### Desktop/GUI
- `DEBUG_DESKTOP` - Desktop manager
- `DEBUG_LUA` - Lua runtime

## Conversion Examples

### Example 1: RTL8139 Driver

```c
// Add include at top of file
#include "debug_config.h"

// Convert verbose debug prints
// OLD:
serial_printf("RTL8139: Processed received packet of %d bytes, EtherType=0x%04x\n", length, ethertype);

// NEW:
SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: Processed received packet of %d bytes, EtherType=0x%04x\n", length, ethertype);

// Keep important error messages as regular printf (always shown):
printf("RTL8139: Failed to initialize device\n");
```

### Example 2: Network Stack

```c
// In net_ip.c
#include "debug_config.h"

// OLD:
printf("IP: Received packet from %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

// NEW:
DEBUG_PRINT(NET_IP, "IP: Received packet from %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
```

### Example 3: Conditional Blocks

For larger debug blocks, use if statement:

```c
#if DEBUG_RTL8139
    printf("RTL8139: Dumping packet contents:\n");
    for (int i = 0; i < length; i++) {
        printf("%02x ", buffer[i]);
    }
    printf("\n");
#endif
```

## Best Practices

1. **Keep error messages always visible** - Use regular `printf()` for errors, not debug macros
2. **Use debug macros for verbose output** - Packet receipts, routine operations, etc.
3. **Use SERIAL_DEBUG_PRINT for low-level code** - Interrupt handlers, early boot code
4. **Group related output** - All RTL8139 debug under DEBUG_RTL8139, etc.

## Performance Benefits

- **Zero overhead when disabled** - Debug code is compiled out entirely
- **No runtime checks** - All decisions made at compile time
- **Cleaner logs** - Only see what you're debugging
- **Faster boot** - Less printf() calls means faster kernel startup

## Example: Debugging Network Latency

```c
// In debug_config.h
#define DEBUG_RTL8139           1   // See packet arrival
#define DEBUG_NET_IP            1   // See IP processing
#define DEBUG_NET_ICMP          1   // See ICMP replies
#define DEBUG_NET_ARP           0   // Disable ARP spam
#define DEBUG_NET_TCP           0   // Disable TCP
```

This gives you clean logs showing only the ping path!

## Migration Status

- ✅ `src/rtl8139.c` - Partially converted (RX path)
- ⏳ `src/net_ip.c` - Needs conversion
- ⏳ `src/net_icmp.c` - Needs conversion
- ⏳ Other files - Gradual conversion

Convert files as you work on them - no need to convert everything at once!
