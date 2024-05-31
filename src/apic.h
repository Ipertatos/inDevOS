#pragma once

#include "types.h"
#include "registers.h"
#include "acpi.h"
typedef struct{           // https://web.archive.org/web/20161130153145/http://download.intel.com/design/chipsets/datashts/29056601.pdf pg. 11 
    uint8_t vector_num;             // 0x10 to 0xFE
    uint8_t delivery_mode : 3;      // 0 normal, 1 low prio., 2 sys manag. int, 4 nmi, 5 init, 7 external
    uint8_t destination_mode : 1;   // 0 physical, 1 logical
    uint8_t delivery_status : 1;    // 0 idle, 1 busy
    uint8_t intpol : 1;             // 0 high, 1 low - specifies polarity of interrupt
    uint8_t remote_irr : 1;         // 1 int recieved, 0 EOI
    uint8_t trigger_mode : 1;       // 0 edge sens., 1 level sens.
    uint8_t intmask : 1;            // interrupt mask
    uint64_t reserved : 39;         // reserved
    uint8_t destination;            // low. 4 bits contain APIC id
} __attribute((packed))redirection_entry_t;

typedef struct{
    uint64_t *address;
    uint8_t type;
    uint8_t apic_id;
    bool present;
} __attribute((packed))madt_ics_t;

void get_madt_tables(madt_t *madt);
uint32_t ioapic_read(madt_ioapic_t *pApic, uint8_t reg);
void ioapic_write(madt_ioapic_t *pApic, uint8_t reg, uint32_t data);
void ioapic_redt_write(void *ioapic_addr, uint8_t reg, uint64_t value);
uint32_t apic_read(void* apic_base, uint32_t reg);
void apic_write(void* apic_base, uint32_t reg, uint32_t data);
void apic_send_inter(void* apic_base, uint8_t apic, uint8_t vector);
void apic_eoi();
void apic_asleep();
void apic_timer();
void ioapic_configure_entry(uint64_t* addr, uint8_t reg, uint64_t val);
void ioapic_redirect_irq(uint32_t lapic_id, uint8_t irq, uint8_t vector, bool status);
madt_ioapic_t* find_gsi(int leg_pin);
void ps2_int_init();
void calibrate_timer(madt_t *madt);
void init_apic(madt_t *madt);
void init_apic_ap();