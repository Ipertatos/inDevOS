# inDevOS Driver Manager

A comprehensive driver management system for inDevOS that allows easy switching between drivers and loading drivers from disk.

## Features

### 🚀 Core Features
- **Automatic Driver Discovery**: Automatically detects and loads drivers for detected hardware
- **Driver State Management**: Load, unload, enable, disable, and reset drivers
- **Driver Status Monitoring**: Real-time status and error tracking
- **Command-Line Interface**: Easy-to-use commands for driver management
- **Modular Architecture**: Clean separation between driver manager and individual drivers

### 🔧 Supported Drivers
- **Storage Drivers**:
  - NVMe storage controller
  - AHCI SATA controller
  - IDE controller
- **Network Drivers**:
  - Intel E1000 network adapter
  - Realtek RTL8139 network adapter
- **Graphics Drivers**:
  - AMD GPU driver
- **USB Drivers**:
  - USB host controller

### 📁 File Structure
```
src/
├── driver_manager.h          # Main driver manager header
├── driver_manager.c          # Core driver manager implementation
├── driver_commands.c         # Command-line interface
├── driver_adapters.h         # Driver adapter system header
├── driver_adapters.c         # Driver adapter implementations
└── driver_file_format.h      # Driver file format specification
```

## Usage

### Command-Line Interface

The driver manager provides a comprehensive command-line interface:

```bash
# List all registered drivers
driver list

# Show status of a specific driver
driver status <id>

# Enable a driver
driver enable <id>

# Disable a driver
driver disable <id>

# Reset a driver
driver reset <id>

# Unload a driver
driver unload <id>

# Probe all drivers
driver probe

# Auto-discover and load drivers
driver discover

# Show help
driver help
```

### Example Session

```bash
# List all drivers
driver list

# Output:
# Driver Manager - Registered Drivers:
# =====================================
# ID: 1 | NVMe Driver | Storage | Initialized | 8086:5845
# ID: 2 | AHCI Driver | Storage | Running | 8086:2922
# ID: 3 | E1000 Driver | Network | Initialized | 8086:100E

# Check status of NVMe driver
driver status 1

# Enable the E1000 network driver
driver enable 3

# Disable the AHCI driver
driver disable 2
```

## Architecture

### Driver Manager Core

The driver manager consists of several key components:

1. **Driver Registration System**: Manages driver metadata and operations
2. **Driver Instance Management**: Tracks loaded driver instances
3. **State Management**: Handles driver lifecycle (unloaded → loaded → initialized → running)
4. **Error Tracking**: Monitors and reports driver errors

### Driver Adapters

Driver adapters bridge existing drivers with the new driver manager:

- **NVMe Adapter**: Integrates NVMe storage driver
- **AHCI Adapter**: Integrates AHCI SATA driver
- **IDE Adapter**: Integrates IDE controller driver
- **E1000 Adapter**: Integrates Intel E1000 network driver
- **RTL8139 Adapter**: Integrates Realtek RTL8139 network driver
- **AMD GPU Adapter**: Integrates AMD GPU driver
- **USB Adapter**: Integrates USB host controller driver

### Driver States

Drivers can be in one of several states:

- **Unloaded**: Driver is registered but not loaded
- **Loaded**: Driver is loaded into memory
- **Initialized**: Driver is initialized and ready
- **Running**: Driver is active and operational
- **Error**: Driver encountered an error
- **Disabled**: Driver is disabled

## Implementation Details

### Driver Registration

Drivers are registered using the `driver_registration_t` structure:

```c
typedef struct {
    driver_info_t info;        // Driver metadata
    driver_ops_t ops;          // Driver operations
    size_t private_data_size;  // Size of private data
} driver_registration_t;
```

### Driver Operations

Each driver must implement a set of operations:

```c
typedef struct {
    int (*init)(struct pci_device *dev);
    int (*deinit)(void);
    int (*probe)(struct pci_device *dev);
    int (*enable)(void);
    int (*disable)(void);
    int (*reset)(void);
    int (*suspend)(void);
    int (*resume)(void);
    void (*status)(void);
} driver_ops_t;
```

### Auto-Discovery

The driver manager automatically discovers hardware and loads appropriate drivers:

1. **PCI Device Scanning**: Scans all PCI devices
2. **Driver Matching**: Matches devices to registered drivers
3. **Driver Loading**: Loads and initializes matching drivers
4. **State Management**: Tracks driver states and errors

## Future Enhancements

### Driver File Format

Planned support for loading drivers from disk files:

```c
typedef struct {
    uint32_t magic;           // Magic number (DRIV)
    uint16_t version;         // File format version
    uint16_t header_size;     // Size of header
    uint32_t driver_size;     // Size of driver data
    uint32_t checksum;        // Data checksum
    char name[64];           // Driver name
    char description[256];   // Driver description
    uint32_t flags;          // Driver flags
} driver_file_header_t;
```

### Hot-Plug Support

Future support for hot-plugging devices:

- Dynamic driver loading/unloading
- Automatic device detection
- Runtime driver switching

### Driver Dependencies

Planned support for driver dependencies:

- Dependency resolution
- Load order management
- Conflict detection

## Building and Testing

### Build the System

```bash
make clean
make
```

### Run Tests

```bash
./scripts/test_driver_manager.sh
```

### Manual Testing

1. Start the OS: `make run`
2. Use driver commands to manage drivers
3. Test driver enable/disable functionality
4. Monitor driver status and errors

## Integration

The driver manager is integrated into the kernel initialization:

```c
// In kernel.c
driver_manager_init();
driver_adapters_init();
driver_adapters_register_all();
driver_auto_discover();
```

## Benefits

### For Developers
- **Modular Design**: Easy to add new drivers
- **Standardized Interface**: Consistent driver API
- **Error Handling**: Comprehensive error tracking
- **Debugging Support**: Detailed status information

### For Users
- **Easy Management**: Simple command-line interface
- **Automatic Discovery**: No manual driver loading required
- **Status Monitoring**: Real-time driver status
- **Flexibility**: Easy driver switching

## Contributing

To add a new driver:

1. Create a driver adapter in `driver_adapters.c`
2. Register the driver in `driver_adapters_register_all()`
3. Implement required driver operations
4. Test the driver integration

## License

This driver manager is part of inDevOS and follows the same license terms. 