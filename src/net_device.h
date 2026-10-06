#pragma once

#include <stdint.h>
#include "pci.h"

// Network device interface
typedef struct net_device net_device_t;

// Network device operations structure
typedef struct {
    int (*init)(void);
    int (*send_packet)(const uint8_t *data, uint32_t length);
    int (*receive_packet)(uint8_t *buffer, uint32_t max_length);
    void (*get_mac_address)(uint8_t *mac);
    int (*has_pending_receive)(void);
    void (*handle_interrupt)(void);
    const char* (*get_driver_name)(void);
    int (*is_initialized)(void);
} net_device_ops_t;

// Network device structure
struct net_device {
    const char *name;
    net_device_ops_t ops;
    uint8_t mac_address[6];
    uint8_t ip_address[4];
    uint8_t subnet_mask[4];
    uint8_t gateway[4];
    uint8_t dns_server[4];
    struct pci_device *pci_dev;
    int initialized;
    void *driver_data; // Pointer to driver-specific data (e1000_dev, rtl8139_dev, etc.)
};

// Global current network device
extern net_device_t *current_net_device;

// Network device management functions
void net_device_init(void);
/**
 * Probe PCI for a supported ethernet adapter and init + register the first match.
 * Priority: Intel E1000 family, then RTL8139, then AMD PCnet.
 * Returns 0 if a driver was registered and initialized, -1 if none found.
 */
int net_device_pci_bringup(void);
int net_device_register(net_device_t *dev);
void net_device_set_current(net_device_t *dev);
net_device_t* net_device_get_current(void);

// Network device interface functions (these will delegate to the current device)
int net_send_packet(const uint8_t *data, uint32_t length);
int net_receive_packet(uint8_t *buffer, uint32_t max_length);
void net_get_mac_address(uint8_t *mac);
void net_get_ip_address(uint8_t *ip);
void net_set_ip_address(const uint8_t *ip);
void net_get_subnet_mask(uint8_t *mask);
void net_set_subnet_mask(const uint8_t *mask);
void net_get_gateway(uint8_t *gw);
void net_set_gateway(const uint8_t *gw);
void net_get_dns_server(uint8_t *dns);
void net_set_dns_server(const uint8_t *dns);
int net_has_pending_receive(void);
const char* net_get_driver_name(void);
int net_is_initialized(void); 