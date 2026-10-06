#pragma once

#include "types.h"
#include "pci.h"
#include "atomic.h"
#include <stdbool.h>

// Driver types
typedef enum {
    DRIVER_TYPE_STORAGE,
    DRIVER_TYPE_NETWORK,
    DRIVER_TYPE_GPU,
    DRIVER_TYPE_USB,
    DRIVER_TYPE_AUDIO,
    DRIVER_TYPE_INPUT,
    DRIVER_TYPE_UNKNOWN
} driver_type_t;

// Driver states
typedef enum {
    DRIVER_STATE_UNLOADED,
    DRIVER_STATE_LOADED,
    DRIVER_STATE_INITIALIZED,
    DRIVER_STATE_RUNNING,
    DRIVER_STATE_ERROR,
    DRIVER_STATE_DISABLED
} driver_state_t;

// Driver information structure
typedef struct driver_info {
    char name[64];
    char description[256];
    driver_type_t type;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass_code;
    uint8_t prog_if;
    uint32_t version;
    bool supports_hotplug;
    bool requires_interrupts;
} driver_info_t;

// Driver operations structure
typedef struct driver_ops {
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

// Driver instance structure
typedef struct driver_instance {
    uint32_t id;
    driver_info_t info;
    driver_ops_t ops;
    driver_state_t state;
    struct pci_device *pci_dev;
    void *private_data;
    uint64_t load_time;
    uint32_t error_count;
    char last_error[256];
    struct driver_instance *next;
} driver_instance_t;

// Driver manager structure
typedef struct {
    driver_instance_t *drivers;
    uint32_t driver_count;
    uint32_t next_driver_id;
    bool initialized;
    spinlock_t lock;
} driver_manager_t;

// Driver registration structure
typedef struct {
    driver_info_t info;
    driver_ops_t ops;
    size_t private_data_size;
} driver_registration_t;

// Function declarations
void driver_manager_init(void);
int driver_register(const driver_registration_t *reg);
int driver_unregister(uint32_t driver_id);
int driver_load_from_disk(const char *filename);
int driver_unload(uint32_t driver_id);
int driver_enable(uint32_t driver_id);
int driver_disable(uint32_t driver_id);
int driver_reset(uint32_t driver_id);
int driver_probe_all(void);
void driver_list_all(void);
void driver_status(uint32_t driver_id);
driver_instance_t* driver_find_by_id(uint32_t driver_id);
driver_instance_t* driver_find_by_pci(struct pci_device *dev);
driver_instance_t* driver_find_by_name(const char *name);
driver_instance_t* driver_find_by_type(driver_type_t type);

// Driver discovery and auto-loading
int driver_auto_discover(void);
int driver_load_matching(struct pci_device *dev);

// Driver file format functions
int driver_save_to_disk(uint32_t driver_id, const char *filename);
int driver_load_from_memory(const void *data, size_t size);

// Global driver manager instance
extern driver_manager_t global_driver_manager;

// Predefined driver registrations
extern const driver_registration_t storage_drivers[];
extern const driver_registration_t network_drivers[];
extern const driver_registration_t gpu_drivers[];
extern const driver_registration_t usb_drivers[];

// Driver manager commands
void cmd_driver_help(void);
void cmd_driver_list(void);
void cmd_driver_load(const char *args);
void cmd_driver_unload(const char *args);
void cmd_driver_enable(const char *args);
void cmd_driver_disable(const char *args);
void cmd_driver_status(const char *args);
void cmd_driver_reset(const char *args);
void cmd_driver_probe(const char *args);
void cmd_driver_save(const char *args); 