#include "cpuid_def.h"
#include "apic.h"
#include "cpuid.h"
#include "msr.h"
#include "keyboard.h"


#define LAPIC_ID_REG        0x20            // lapic id register
#define LAPIC_VERSION_REG   0x30            // lapic version register
 
#define LAPIC_EOI_REG       0xB0            // end of interrupt register
#define LAPIC_SIVR_REG      0xF0            // spurious interrupt vector register

#define LAPIC_ICR_LOW       0x300           // low 32 bits of the ICR 
#define LAPIC_ICR_HIGH      0x310           // high 32 bits of the ICR (xAPIC only, 2xAPIC has a 64-bit MSR for the ICR)

#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360

#define LAPIC_TIMERDIV_REG  0x3E0
#define LAPIC_LVTTIMER_REG  0x320
#define LAPIC_INITCNT_REG   0x380
#define LAPIC_CURCNT_REG    0x390
#define LAPIC_PERIODIC      0x20000

#define LAPIC_EOI_COMMAND   0x0

#define IA32_APIC_BASE_MSR  0x1B

#define IOAPICID_REG        0x0
#define IOAPICVER_REG       0x1
#define IOAPICARB_REG       0x2
#define IOAPICREDTBL_REG(n) (0x10 + 2 * n)  // n being which redtbl register (0-24)

#define ICR_DELMODE_INIT            0x280
#define ICR_DELMODE_STARTUP         0x300

#define ICR_DEST_ALLEXSELF          0xC0000

uint32_t *lapic_address;
uint32_t lapic_addr;
uint64_t *ioapic_address;

extern madt_t *madt;
extern uint64_t hhdmoffset;

uint64_t apic_timer_ticks;

madt_ics_t ics_arr[64];

////////////////////////

void get_madt_tables(madt_t *madt)
{
   int i =0;
    madt_record_t *cur_ics = &madt->first_ics;
    int length = madt->header.length - sizeof(madt_t) +2;
    while (length > 0)
    {
        switch (cur_ics->entry_type)
        {
        case 0:
            ;
            madt_plapic_t *plapic = (madt_plapic_t*)cur_ics;
            if(plapic->flags == 0)
                break;
            else {
                printf("apic: found cpu: {dn}", plapic->apicID);
                ics_arr[i].address = (uint64_t*)cur_ics;
                ics_arr[i].type = cur_ics->entry_type;
                ics_arr[i].apic_id = plapic->apicID;
                ics_arr[i].present = true;
                i++;
                break;
            }
            break;
        case 1:
            ;
            madt_ioapic_t *ioapic = (madt_ioapic_t*)cur_ics;
            ics_arr[i].address = (uint64_t*)cur_ics;
            ics_arr[i].type = cur_ics->entry_type;
            i++;
            ioapic_address = (uint64_t*)ioapic->ioapicaddr;
            break;
        case 2:
            ;
            ics_arr[i].address = (uint64_t*)cur_ics;
            ics_arr[i].type = cur_ics->entry_type;
            i++;
            break;
        }
        length -= cur_ics->record_length;
        cur_ics = (madt_record_t*)((uint64_t)cur_ics + cur_ics->record_length);
    }


}

int ioapic_read(void *ioapic_addr, uint8_t reg){
    uint32_t volatile *ioapicptr = (uint32_t volatile*)ioapic_addr;
    ioapicptr[0] = (reg & 0xFF);
    return ioapicptr[4];
}


void ioapic_write(void *ioapic_addr, uint8_t reg, uint32_t data){
    uint32_t volatile *ioapicptr = (uint32_t volatile*)ioapic_addr;
    ioapicptr[0] = (reg & 0xFF);
    ioapicptr[4] = data;
}

void ioapic_redt_write(void *ioapic_addr, uint8_t reg, uint64_t value){
    uint64_t volatile *ioapicptr = (uint64_t volatile*)ioapic_addr;
    ioapicptr[0] = (reg & 0xFF);
    ioapicptr[4] = value;
}

uint32_t apic_read(void* apic_base, uint32_t reg){
    return *((volatile uint32_t*)(apic_base + reg));
}

void apic_write(void* apic_base, uint32_t reg, uint32_t data){
    extern uint64_t hddmoffset;
    *((volatile uint32_t*)(apic_base + reg + hhdmoffset)) = data;
}

void apic_send_inter(void* apic_base, uint8_t apic, uint8_t vector){
    apic_write(apic_base, LAPIC_ICR_HIGH, ((uint32_t)apic) << 24);
    apic_write(apic_base, LAPIC_ICR_LOW, vector);
}

void apic_eoi(){
    apic_write((uint32_t*)lapic_address, LAPIC_EOI_REG, LAPIC_EOI_COMMAND);
}

void apic_asleep(int ms){
    int curcnt = apic_timer_ticks;
    while(apic_timer_ticks - curcnt < ms){
        asm("nop");
    }
}

void apic_timer(){
    apic_timer_ticks++;
    apic_eoi();
}

void ioapic_configure_entry(uint64_t* addr, uint8_t reg, uint64_t val){
    ioapic_write(addr, IOAPICREDTBL_REG(reg), (uint32_t)val);
    ioapic_write(addr, IOAPICREDTBL_REG(reg) + 1, (uint32_t)(val >> 32));
}

int find_gsi(int leg_pin){
    for(int i = 0; i < 64; i++){ //no reason to have more than 64 ics entries
        if(ics_arr[i].type == 2){
            madt_iso_t *iso = (madt_iso_t*)ics_arr[i].address;
            if(iso->source == leg_pin){
                printf("apic: found legacy pin {d} connected to IOAPIC {dn}", leg_pin, iso->gsi);
                return iso->gsi;//we found a legacy pin connected to IOAPIC
            }
        }
    }
    return leg_pin; //no IOAPIC found, return the legacy pin
}


//we can do ps2 here
void ps2_int_init(){
    log_info("Initializing PS/2 keyboard\n");
    int gsi = find_gsi(1);

    keyboard_init();//TODO
    ioapic_configure_entry(ioapic_address, gsi, 0 << 16);
    ioapic_configure_entry(ioapic_address, gsi , 33);
    printf("ps2: configured IOAPIC redirection entry{n}");
    log_success("PS/2 keyboard initialized");
} 

void calibrate_timer(madt_t *madt){
    lapic_address= (uint32_t)madt->lapicaddr;
    apic_write(lapic_address,LAPIC_TIMERDIV_REG,3); //divisor 16
    apic_write(lapic_addr,LAPIC_INITCNT_REG,0xFFFFFFFF);
    pmt_delay(50000);
    apic_write(lapic_address,LAPIC_TIMERDIV_REG,0x10000); //0x10000 = masked, sdm
    uint32_t calibarion = 0xffffffff - apic_read(lapic_address, LAPIC_CURCNT_REG);
    apic_write(lapic_address,LAPIC_LVTTIMER_REG,172 | LAPIC_PERIODIC);
    apic_write(lapic_address, LAPIC_TIMERDIV_REG,0x3); // 16
    apic_write(lapic_address, LAPIC_INITCNT_REG,calibarion);
}
void init_apic(madt_t *madt, uint64_t hhdmoffset){
    asm("cli");
    get_madt_tables(madt);
    
    printf("apic: MADT tables listed through{n}");
    printf("apic: writing to SIV register{n}");
    lapic_addr = madt->lapicaddr;
    uint32_t spurius_reg = apic_read((void*)madt->lapicaddr + hhdmoffset , 0xF0);
    printf("madt lapic adr: 0x{xn}", lapic_addr);
    apic_write((void*)madt->lapicaddr, 0xF0,spurius_reg | (0x100));
    log_success("LAPIC initialized");
    calibrate_timer(madt);
    for(int i=0 ; i != ((ioapic_read(ioapic_address,0x2) >> 16)&0xFF); i++){
        ioapic_configure_entry(ioapic_address,i,1<<16);
    }
    asm("sti");
    ps2_int_init();
    log_success("IOAPIC initialized");
}

void int_apic_ap(){
    uint32_t spurius_reg = apic_read((uint32_t*)(lapic_addr), 0xF0);
    calibrate_timer(madt);
}