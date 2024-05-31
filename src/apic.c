#include "cpuid_def.h"
#include "apic.h"
#include "acpi.h"
#include "cpuid.h"
#include "msr.h"
#include "keyboard.h"
#include "hpet.h"
#include "types.h"
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
uint32_t *lapic_addr;
uint64_t *ioapic_address;

extern madt_t *madt;
extern uint64_t vmm_higher_half_offset;

uint64_t apic_timer_ticks;

madt_ics_t ics_arr[128];

madt_ioapic_t *ioapic_list[128];
madt_iso_t *iso_list[128];

////////////////////////
void add_ioapic(madt_ioapic_t *pIOApic)
{
    for (int i = 0; i < 128; i++) {
        if (ioapic_list[i] == NULL) {
            ioapic_list[i] = pIOApic;
            return;
        }
    }
}

void add_iso(madt_iso_t *pISO)
{
    for (int i = 0; i < 128; i++) {
        if (iso_list[i] == NULL) {
            printf("ISO OVERRIDE: {d} -> {d}.{n}", pISO->source, pISO->gsi);
            iso_list[i] = pISO;
            return;
        }
    }
}

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
            if(plapic->flags == 0){
                break;
            }else {
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
            add_ioapic((madt_ioapic_t*)cur_ics);
            break;
        case 2:
            ;
            ics_arr[i].address = (uint64_t*)cur_ics;
            ics_arr[i].type = cur_ics->entry_type;
            add_iso((madt_iso_t*)cur_ics);
            i++;
            break;
        }
        length -= cur_ics->record_length;
        cur_ics = (madt_record_t*)((uint64_t)cur_ics + cur_ics->record_length);
    }


}

uint32_t ioapic_read(madt_ioapic_t *pApic, const uint8_t reg){

    uint64_t ioapicptr = (uint64_t)pApic->ioapicaddr + vmm_higher_half_offset;
    *(volatile uint32_t*)(ioapicptr) = reg;
    return *(volatile uint32_t*)(ioapicptr + 0x10);

}


void ioapic_write(madt_ioapic_t *pApic, uint8_t reg, uint32_t data){
    uint64_t ioapicptr = (uint64_t)pApic->ioapicaddr + vmm_higher_half_offset;
    *(volatile uint32_t*)(ioapicptr) = reg;
    *(volatile uint32_t*)(ioapicptr + 0x10) = data;
}

void ioapic_redt_write(void *ioapic_addr, uint8_t reg, uint64_t value){
    uint64_t volatile *ioapicptr = (uint64_t volatile*)ioapic_addr;
    ioapicptr[0] = (reg & 0xFF);
    ioapicptr[4] = value;
}

uint32_t apic_read(void* apic_base, uint32_t reg){
    return *((volatile uint32_t*)(vmm_higher_half_offset + apic_base + reg));
}

void apic_write(void* apic_base, uint32_t reg, uint32_t data){
    *((volatile uint32_t*)(apic_base + reg + vmm_higher_half_offset)) = data;
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

void ioapic_redirect_irq(uint32_t lapic_id, uint8_t vector, uint8_t irq, bool status){
    for (int i = 0; i < 128; i++) {
        madt_iso_t* pISO = iso_list[i];
        if (pISO == NULL) {
            // Reach end of list.
            break;
        }

        // Does this override entry affect the IRQ we want to redirect?
        if (irq == pISO->source) {
            printf("I/O APIC using override from {d} to {d}.\n", irq, pISO->gsi);
            _ioapic_redirect_gsi(lapic_id, vector, pISO->gsi, pISO->flags, status);
            return;
        }
    }

    // No overrides.
    _ioapic_redirect_gsi(lapic_id, vector, irq, 0, status);
}
static uint64_t ioapic_max_gsi(struct madt_ioapic_t *pIOApic)
{
    uint64_t max = (ioapic_read(pIOApic, 0x01) & 0xFF0000) >> 16;
    return max;
}
madt_ioapic_t* find_gsi(int leg_pin){
    for(int i = 0; i < 128; i++){ //no reason to have more than 64 ics entries
        madt_ioapic_t *pIOApic = ioapic_list[i];

        if(pIOApic == NULL){
            return NULL;
        }

        if(leg_pin >= pIOApic->gsi && leg_pin < pIOApic->gsi + ioapic_max_gsi(pIOApic)){
            return pIOApic;
        }

    }
    return NULL; //no IOAPIC found, return the legacy pin
}


void _ioapic_redirect_gsi(uint32_t lapic_id, uint8_t vector, uint8_t gsi, uint16_t flags, bool status)
{
    madt_ioapic_t *pIOApic = find_gsi(gsi);

    uint64_t redirect = vector;

    // Set the interrupt polarity to active low.
    if ((flags & (1 << 1)) != 0) {
        redirect |= (1 << 13);
    }

    // Set interrupt to be level-triggered (not edge-triggered).
    if ((flags & (1 << 3)) != 0) {
        redirect |= (1 << 15);
    }

    // The enabled mask is on the 16th bit.
    if (!status) {
        redirect |= (1 << 16);
    }

    // Put the APIC ID in to the upper 8 bits - this selects which CPU handles the interrupt.
    redirect |= (uint64_t)lapic_id << 56;

    // Get the location inside the redirect table for the GSI.
    uint32_t io_redirect_table_reg = (gsi - pIOApic->gsi) * 2 + 16;

    // The high and low of the table entry get written in 2 seperate write commands.
    ioapic_write(pIOApic, io_redirect_table_reg, (uint32_t)redirect);
    ioapic_write(pIOApic, io_redirect_table_reg + 1, (uint32_t)(redirect >> 32));

    printf("Applied I/O APIC redirect: Vector {d}, GSI {d}.{n}}", vector, gsi);
}


//we can do ps2 here
void ps2_int_init(){
    log_info("Initializing PS/2 keyboard");
    keyboard_init();//TODO

    //_ioapic_redirect_gsi


    printf("ps2: configured IOAPIC redirection entry{n}");
    log_success("PS/2 keyboard initialized");
} 

void calibrate_timer(madt_t *madt){
    lapic_address= (uint32_t)madt->lapicaddr;
    apic_write(lapic_address,LAPIC_TIMERDIV_REG,3); //divisor 16
    apic_write(lapic_address,LAPIC_INITCNT_REG,0xFFFFFFFF);
    pmt_delay(50000);
    apic_write(lapic_address,LAPIC_TIMERDIV_REG,0x10000); //0x10000 = masked, sdm
    uint32_t calibarion = 0xffffffff - apic_read(lapic_address, LAPIC_CURCNT_REG);
    apic_write(lapic_address,LAPIC_LVTTIMER_REG,172 | LAPIC_PERIODIC);
    apic_write(lapic_address, LAPIC_TIMERDIV_REG,0x3); // 16
    apic_write(lapic_address, LAPIC_INITCNT_REG,calibarion);
}
void init_apic(madt_t *madt){
    asm("cli");
    get_madt_tables(madt);
    
    printf("apic: MADT tables listed through{n}");
    printf("apic: writing to SIV register{n}");
        hpet_init();
    lapic_addr = madt->lapicaddr;
    uint32_t spurius_reg = apic_read((void*)madt->lapicaddr, LAPIC_SIVR_REG);
    printf("madt lapic adr: 0x{xn}", lapic_addr);
    apic_write((void*)madt->lapicaddr, LAPIC_SIVR_REG,spurius_reg | (0x100));
    log_success("LAPIC initialized");
    calibrate_timer(madt);
   // for(int i=0 ; i != ((ioapic_read(ioapic_list[0],IOAPICVER_REG) >> 16)&0xFF); i++){
    //    ioapic_configure_entry(ioapic_address[0],i,1<<16);
    //}

    apic_write(lapic_addr, LAPIC_INITCNT_REG,0);
    apic_write(lapic_addr, LAPIC_LVTTIMER_REG, 0x10000);

    //asm("sti");
    ps2_int_init();
    log_success("IOAPIC initialized");
}

void int_apic_ap(){
    uint32_t spurius_reg = apic_read((uint32_t*)(lapic_addr), 0xF0);
    calibrate_timer(madt);
}