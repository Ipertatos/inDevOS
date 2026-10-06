#include "net_device.h"
#include "mem.h"
#include "string.h"
#include "e1000.h"
#include "rtl8139.h"
#include "pcnet.h"
#include "string.h"

// Global current network device
net_device_t *current_net_device = NULL;

// Initialize the network device subsystem
void net_device_init(void) {
    current_net_device = NULL;
    printf("Network device abstraction layer initialized\n");
}

int net_device_pci_bringup(void)
{
    e1000_init();
    if (e1000_dev.initialized) {
        printf("net: using Intel E1000\n");
        return e1000_register_net_device();
    }
    rtl8139_init();
    if (rtl8139_dev.initialized) {
        printf("net: using Realtek RTL8139\n");
        return rtl8139_register_net_device();
    }
    pcnet_init();
    if (pcnet_dev.initialized) {
        printf("net: using AMD PCnet\n");
        return pcnet_register_net_device();
    }
    printf("net: no supported PCI ethernet adapter initialized\n");
    return -1;
}

// Register a network device
int net_device_register(net_device_t *dev) {
    if (!dev) {
        printf("Net device: Cannot register NULL device\n");
        return -1;
    }
    
    printf("Net device: Registered '%s' driver\n", dev->name);
    
    // If this is the first device or current device is not initialized, make it current
    if (!current_net_device || !current_net_device->initialized) {
        net_device_set_current(dev);
    }
    
    return 0;
}

// Set the current active network device
void net_device_set_current(net_device_t *dev) {
    if (!dev) {
        printf("Net device: Cannot set NULL device as current\n");
        return;
    }
    
    current_net_device = dev;
    printf("Net device: Set '%s' as current network device\n", dev->name);

    // Do NOT wipe the device's IP/gateway/DNS configuration here: driver
    // structs start zeroed, and re-selecting a device (e.g. after a second
    // registration pass) must preserve config already obtained via DHCP or
    // static assignment.
}

// Get the current active network device
net_device_t* net_device_get_current(void) {
    return current_net_device;
}

// Network device interface functions - these delegate to the current device

int net_send_packet(const uint8_t *data, uint32_t length) {
    if (!current_net_device || !current_net_device->ops.send_packet) {
        printf("Net device: No active device or send_packet not implemented\n");
        return -1;
    }
    
    return current_net_device->ops.send_packet(data, length);
}

int net_receive_packet(uint8_t *buffer, uint32_t max_length) {
    if (!current_net_device || !current_net_device->ops.receive_packet) {
        return 0; // No packets if no device
    }
    
    return current_net_device->ops.receive_packet(buffer, max_length);
}

void net_get_mac_address(uint8_t *mac) {
    if (!current_net_device) {
        memset(mac, 0, 6);
        return;
    }
    
    if (current_net_device->ops.get_mac_address) {
        current_net_device->ops.get_mac_address(mac);
    } else {
        memcpy(mac, current_net_device->mac_address, 6);
    }
}

void net_get_ip_address(uint8_t *ip) {
    if (!current_net_device) {
        memset(ip, 0, 4);
        return;
    }
    
    memcpy(ip, current_net_device->ip_address, 4);
}

void net_set_ip_address(const uint8_t *ip) {
    if (!current_net_device || !ip) {
        return;
    }
    
    memcpy(current_net_device->ip_address, ip, 4);
}

void net_get_subnet_mask(uint8_t *mask) {
    if (!current_net_device) { memset(mask, 0, 4); return; }
    memcpy(mask, current_net_device->subnet_mask, 4);
}

void net_set_subnet_mask(const uint8_t *mask) {
    if (!current_net_device || !mask) return;
    memcpy(current_net_device->subnet_mask, mask, 4);
}

void net_get_gateway(uint8_t *gw) {
    if (!current_net_device) { memset(gw, 0, 4); return; }
    memcpy(gw, current_net_device->gateway, 4);
}

void net_set_gateway(const uint8_t *gw) {
    if (!current_net_device || !gw) return;
    memcpy(current_net_device->gateway, gw, 4);
}

void net_get_dns_server(uint8_t *dns) {
    if (!current_net_device) { memset(dns, 0, 4); return; }
    memcpy(dns, current_net_device->dns_server, 4);
}

void net_set_dns_server(const uint8_t *dns) {
    if (!current_net_device || !dns) return;
    memcpy(current_net_device->dns_server, dns, 4);
}

int net_has_pending_receive(void) {
    if (!current_net_device || !current_net_device->ops.has_pending_receive) {
        return 0;
    }
    
    return current_net_device->ops.has_pending_receive();
}

const char* net_get_driver_name(void) {
    if (!current_net_device) {
        return "None";
    }
    
    if (current_net_device->ops.get_driver_name) {
        return current_net_device->ops.get_driver_name();
    }
    
    return current_net_device->name;
}

int net_is_initialized(void) {
    if (!current_net_device) {
        return 0;
    }
    
    if (current_net_device->ops.is_initialized) {
        return current_net_device->ops.is_initialized();
    }
    
    return current_net_device->initialized;
} 