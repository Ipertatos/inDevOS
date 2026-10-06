#pragma once

#include "driver_manager.h"
#include "pci.h"

// Driver adapter structure
typedef struct {
    const char *name;
    driver_registration_t registration;
    int (*init_function)(void);
    int (*deinit_function)(void);
} driver_adapter_t;

// Function declarations for driver adapters
int nvme_driver_adapter_init(struct pci_device *dev);
int nvme_driver_adapter_deinit(void);
int nvme_driver_adapter_probe(struct pci_device *dev);
int nvme_driver_adapter_enable(void);
int nvme_driver_adapter_disable(void);
int nvme_driver_adapter_reset(void);
void nvme_driver_adapter_status(void);

int ahci_driver_adapter_init(struct pci_device *dev);
int ahci_driver_adapter_deinit(void);
int ahci_driver_adapter_probe(struct pci_device *dev);
int ahci_driver_adapter_enable(void);
int ahci_driver_adapter_disable(void);
int ahci_driver_adapter_reset(void);
void ahci_driver_adapter_status(void);

int ide_driver_adapter_init(struct pci_device *dev);
int ide_driver_adapter_deinit(void);
int ide_driver_adapter_probe(struct pci_device *dev);
int ide_driver_adapter_enable(void);
int ide_driver_adapter_disable(void);
int ide_driver_adapter_reset(void);
void ide_driver_adapter_status(void);

int e1000_driver_adapter_init(struct pci_device *dev);
int e1000_driver_adapter_deinit(void);
int e1000_driver_adapter_probe(struct pci_device *dev);
int e1000_driver_adapter_enable(void);
int e1000_driver_adapter_disable(void);
int e1000_driver_adapter_reset(void);
void e1000_driver_adapter_status(void);

int rtl8139_driver_adapter_init(struct pci_device *dev);
int rtl8139_driver_adapter_deinit(void);
int rtl8139_driver_adapter_probe(struct pci_device *dev);
int rtl8139_driver_adapter_enable(void);
int rtl8139_driver_adapter_disable(void);
int rtl8139_driver_adapter_reset(void);
void rtl8139_driver_adapter_status(void);

// RTL810xE driver adapter interface
int rtl810x_driver_adapter_init(struct pci_device *dev);
int rtl810x_driver_adapter_deinit(void);
int rtl810x_driver_adapter_probe(struct pci_device *dev);
int rtl810x_driver_adapter_enable(void);
int rtl810x_driver_adapter_disable(void);
int rtl810x_driver_adapter_reset(void);
void rtl810x_driver_adapter_status(void);

// PCnet driver adapter interface
int pcnet_driver_adapter_init(struct pci_device *dev);
int pcnet_driver_adapter_deinit(void);
int pcnet_driver_adapter_probe(struct pci_device *dev);
int pcnet_driver_adapter_enable(void);
int pcnet_driver_adapter_disable(void);
int pcnet_driver_adapter_reset(void);
void pcnet_driver_adapter_status(void);

int amd_gpu_driver_adapter_init(struct pci_device *dev);
int amd_gpu_driver_adapter_deinit(void);
int amd_gpu_driver_adapter_probe(struct pci_device *dev);
int amd_gpu_driver_adapter_enable(void);
int amd_gpu_driver_adapter_disable(void);
int amd_gpu_driver_adapter_reset(void);
void amd_gpu_driver_adapter_status(void);

int usb_driver_adapter_init(struct pci_device *dev);
int usb_driver_adapter_deinit(void);
int usb_driver_adapter_probe(struct pci_device *dev);
int usb_driver_adapter_enable(void);
int usb_driver_adapter_disable(void);
int usb_driver_adapter_reset(void);
void usb_driver_adapter_status(void);

// Initialize all driver adapters
void driver_adapters_init(void);

// Register all driver adapters with the driver manager
int driver_adapters_register_all(void); 