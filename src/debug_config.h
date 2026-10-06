#pragma once

// Centralized debug configuration for the kernel
// Set to 1 to enable debug output for each subsystem, 0 to disable

// Network subsystems
#define DEBUG_RTL8139           0   // RTL8139 NIC driver (RX/TX/interrupts)
#define DEBUG_NET_IP            0   // IP layer processing
#define DEBUG_NET_ICMP          0   // ICMP echo request/reply
#define DEBUG_NET_ARP           0   // ARP protocol
#define DEBUG_NET_TCP           0   // TCP protocol
#define DEBUG_NET_UDP           0   // UDP protocol
#define DEBUG_NET_DHCP          0   // DHCP client
#define DEBUG_NET_DNS           0   // DNS resolver
#define DEBUG_NET_ETHERNET      0   // Ethernet frame processing
#define DEBUG_NET_DEVICE        0   // Network device abstraction

// Storage subsystems
#define DEBUG_AHCI              0   // AHCI/SATA driver
#define DEBUG_IDE               0   // IDE driver
#define DEBUG_NVME              0   // NVMe driver
#define DEBUG_FAT32             0   // FAT32 filesystem
#define DEBUG_STORAGE           0   // Storage layer

// Hardware drivers
#define DEBUG_PCI               0   // PCI bus enumeration
#define DEBUG_USB               0   // USB subsystem
#define DEBUG_KEYBOARD          0   // Keyboard driver
#define DEBUG_MOUSE             0   // Mouse driver
#define DEBUG_VGA               0   // VGA driver
#define DEBUG_VMWARE_SVGA       0   // VMware SVGA driver
#define DEBUG_AMD_GPU           0   // AMD GPU driver

// System components
#define DEBUG_MEMORY            0   // Memory allocator
#define DEBUG_SCHEDULER         0   // Process/thread scheduler
#define DEBUG_TASK_MANAGER      0   // Cooperative task manager
#define DEBUG_SYSCALL           0   // System calls
#define DEBUG_INTERRUPTS        0   // Interrupt handling
#define DEBUG_TIMER             0   // Timer/HPET

// Desktop/GUI
#define DEBUG_DESKTOP           0   // Desktop manager
#define DEBUG_LUA               0   // Lua runtime

// Convenience macros for conditional debug printing
#define DEBUG_PRINT(subsystem, ...) \
    do { if (DEBUG_##subsystem) printf(__VA_ARGS__); } while(0)

#define SERIAL_DEBUG_PRINT(subsystem, ...) \
    do { if (DEBUG_##subsystem) serial_printf(__VA_ARGS__); } while(0)

// Usage examples:
// DEBUG_PRINT(RTL8139, "RTL8139: Received packet of %d bytes\n", len);
// SERIAL_DEBUG_PRINT(NET_IP, "IP: Processing packet from %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

// Quick enable/disable all network debug
#define DEBUG_ALL_NETWORK       0

#if DEBUG_ALL_NETWORK
#undef DEBUG_RTL8139
#undef DEBUG_NET_IP
#undef DEBUG_NET_ICMP
#undef DEBUG_NET_ARP
#undef DEBUG_NET_TCP
#undef DEBUG_NET_UDP
#undef DEBUG_NET_ETHERNET
#define DEBUG_RTL8139           1
#define DEBUG_NET_IP            1
#define DEBUG_NET_ICMP          1
#define DEBUG_NET_ARP           1
#define DEBUG_NET_TCP           1
#define DEBUG_NET_UDP           1
#define DEBUG_NET_ETHERNET      1
#endif

// Quick enable/disable all storage debug
#define DEBUG_ALL_STORAGE       0

#if DEBUG_ALL_STORAGE
#undef DEBUG_AHCI
#undef DEBUG_IDE
#undef DEBUG_NVME
#undef DEBUG_FAT32
#undef DEBUG_STORAGE
#define DEBUG_AHCI              1
#define DEBUG_IDE               1
#define DEBUG_NVME              1
#define DEBUG_FAT32             1
#define DEBUG_STORAGE           1
#endif
