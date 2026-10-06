#include "cpuid_def.h"
#include "apic.h"
#include "acpi.h"
#include "cpuid.h"
#include "msr.h"
#include "keyboard.h"
#include "hpet.h"
#include "pic.h"
#include "types.h"
#include "atomic.h"
#include "serial.h"

// Forward declarations for internal functions
static void _ioapic_redirect_gsi(uint32_t lapic_id, uint8_t vector, uint8_t gsi, uint16_t flags, bool status);
static uint64_t ioapic_max_gsi(madt_ioapic_t *pIOApic);
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

static atomic_t apic_timer_ticks = {0};

madt_ics_t ics_arr[128];

madt_ioapic_t *ioapic_list[128];
madt_iso_t *iso_list[128];

////////////////////////
void add_ioapic(madt_ioapic_t *pIOApic)
{
    for (int i = 0; i < 128; i++) {
        if (ioapic_list[i] == NULL) {
            ioapic_list[i] = pIOApic;
            DEBUG_SERIAL("Added IOAPIC %d: ID=%d, GSI base=%d, addr=0x%x\n");
            serial_printf("Added IOAPIC %d: ID=%d, GSI base=%d, addr=0x%x\n", 
                         i, pIOApic->ioapicID, pIOApic->gsi, pIOApic->ioapicaddr);
            return;
        }
    }
    DEBUG_SERIAL("ERROR: IOAPIC list full!\n");
}

void add_iso(madt_iso_t *pISO)
{
    for (int i = 0; i < 128; i++) {
        if (iso_list[i] == NULL) {
            serial_printf("ISO OVERRIDE: %d -> %d.\n", pISO->source, pISO->gsi);
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
                serial_printf("apic: found cpu: %d\n", plapic->apicID);
                ics_arr[i].address = cur_ics;
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
            ics_arr[i].address = cur_ics;
            ics_arr[i].type = cur_ics->entry_type;
            i++;
            ioapic_address = (uint64_t*)(uintptr_t)ioapic->ioapicaddr;
            add_ioapic((madt_ioapic_t*)cur_ics);
            break;
        case 2:
            ;
            ics_arr[i].address = cur_ics;
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
    if (pApic == NULL) {
        serial_puts("ERROR: ioapic_read called with NULL pApic\n");
        return 0;
    }

    uint64_t ioapicptr = (uint64_t)pApic->ioapicaddr + vmm_higher_half_offset;
    *(volatile uint32_t*)(ioapicptr) = reg;
    return *(volatile uint32_t*)(ioapicptr + 0x10);

}


void ioapic_write(madt_ioapic_t *pApic, uint8_t reg, uint32_t data){
    if (pApic == NULL) {
        serial_puts("ERROR: ioapic_write called with NULL pApic\n");
        return;
    }
    
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
    uint64_t curcnt = atomic_get(&apic_timer_ticks);
    while(atomic_get(&apic_timer_ticks) - curcnt < ms){
        __asm__ ("nop");
    }
}

void apic_timer(){
    atomic_inc(&apic_timer_ticks);
    hpet_keepalive();
    apic_eoi();
}

void ioapic_configure_entry(madt_ioapic_t *pIOApic, uint8_t reg, uint64_t val){
    if (!pIOApic)
        return;
    ioapic_write(pIOApic, IOAPICREDTBL_REG(reg), (uint32_t)val);
    ioapic_write(pIOApic, IOAPICREDTBL_REG(reg) + 1, (uint32_t)(val >> 32));
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
            serial_printf("I/O APIC using override from %d to %d.\n", irq, pISO->gsi);
            _ioapic_redirect_gsi(lapic_id, vector, pISO->gsi, pISO->flags, status);
            return;
        }
    }

    // No overrides.
    _ioapic_redirect_gsi(lapic_id, vector, irq, 0, status);
}
static uint64_t ioapic_max_gsi(madt_ioapic_t *pIOApic)
{
    if (pIOApic == NULL) {
        serial_puts("ERROR: ioapic_max_gsi called with NULL pIOApic\n");
        return 0;
    }
    
    /* Bits 16:23 are the last redirection index, not the entry count. */
    uint64_t max = (ioapic_read(pIOApic, 0x01) & 0xFF0000) >> 16;
    return max + 1;
}
madt_ioapic_t* find_gsi(int leg_pin){
    DEBUG_SERIAL("Looking for IOAPIC for GSI %d\n");
    serial_printf("Looking for IOAPIC for GSI %d\n", leg_pin);
    
    for(int i = 0; i < 128; i++){ //no reason to have more than 64 ics entries
        madt_ioapic_t *pIOApic = ioapic_list[i];

        if(pIOApic == NULL){
            DEBUG_SERIAL("End of IOAPIC list at index %d\n");
            serial_printf("End of IOAPIC list at index %d\n", i);
            return NULL;
        }

        DEBUG_SERIAL("Checking IOAPIC %d: GSI range %d-%d\n");
        uint64_t max_gsi = ioapic_max_gsi(pIOApic);
        serial_printf("Checking IOAPIC %d: GSI range %d-%d\n", 
                     i, pIOApic->gsi, pIOApic->gsi + max_gsi);

        if(leg_pin >= pIOApic->gsi && leg_pin < pIOApic->gsi + max_gsi){
            DEBUG_SERIAL("Found IOAPIC for GSI %d\n");
            serial_printf("Found IOAPIC for GSI %d\n", leg_pin);
            return pIOApic;
        }

    }
    DEBUG_SERIAL("No IOAPIC found for GSI %d\n");
    serial_printf("No IOAPIC found for GSI %d\n", leg_pin);
    return NULL; //no IOAPIC found, return the legacy pin
}


void _ioapic_redirect_gsi(uint32_t lapic_id, uint8_t vector, uint8_t gsi, uint16_t flags, bool status)
{
    madt_ioapic_t *pIOApic = find_gsi(gsi);
    
    if (pIOApic == NULL) {
        DEBUG_SERIAL("ERROR: No IOAPIC found for GSI %d\n");
        serial_printf("ERROR: No IOAPIC found for GSI %d\n", gsi);
        return;
    }

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

    /* Intel SDM recommended sequence: program the high half (destination) FIRST
     * while the entry is still effectively masked (mask bit was set in previous
     * state or is being written below), then program the low half last (which
     * unmasks). Writing low-then-high opens a small window where an IRQ can be
     * delivered to a stale destination. */
    ioapic_write(pIOApic, io_redirect_table_reg + 1, (uint32_t)(redirect >> 32));
    ioapic_write(pIOApic, io_redirect_table_reg, (uint32_t)redirect);

    /* Read back and verify */
    uint32_t lo = ioapic_read(pIOApic, io_redirect_table_reg);
    uint32_t hi = ioapic_read(pIOApic, io_redirect_table_reg + 1);
    serial_printf("Applied I/O APIC redirect: Vector %d, GSI %d. Readback: lo=0x%08x hi=0x%08x (mask=%d, dest=%u)\n",
                  vector, gsi, lo, hi,
                  (lo >> 16) & 1,
                  (hi >> 24) & 0xFF);
}


//we can do ps2 here
void ps2_int_init(){
    DEBUG_SERIAL("Initializing PS/2 keyboard\n");
    // NOTE: Removed keyboard_init() call from here - it's called separately in kernel.c
    // keyboard_init();

    //_ioapic_redirect_gsi


    DEBUG_SERIAL("ps2: configured IOAPIC redirection entry\n");
    DEBUG_SERIAL("PS/2 keyboard initialized\n");
} 

#define LAPIC_CALIBRATION_US       5000U
#define LAPIC_SCHED_PERIOD_US       500U

static uint32_t lapic_calibrated_count = 0; /* ticks per LAPIC_CALIBRATION_US with divisor 16 */

void calibrate_timer(madt_t *madt){
    lapic_address = (uint32_t*)(uintptr_t)madt->lapicaddr;

    /* Mask the LVT timer first so it can't fire while we're calibrating. */
    apic_write(lapic_address, LAPIC_LVTTIMER_REG, 0x10000); /* masked */
    apic_write(lapic_address, LAPIC_TIMERDIV_REG, 0x3);     /* divisor 16 */
    apic_write(lapic_address, LAPIC_INITCNT_REG, 0xFFFFFFFF);

    /* Sample for 5 ms via the ACPI PM timer. */
    pmt_delay(LAPIC_CALIBRATION_US);

    /* Stop the LAPIC counter by masking the LVT (writing the mask bit to the
     * LVT register, NOT the divide register — the previous code wrote 0x10000
     * to LAPIC_TIMERDIV_REG which silently corrupted the divisor and left the
     * timer running while we sampled). */
    apic_write(lapic_address, LAPIC_LVTTIMER_REG, 0x10000); /* masked */

    uint32_t calibarion = 0xFFFFFFFF - apic_read(lapic_address, LAPIC_CURCNT_REG);
    lapic_calibrated_count = calibarion; /* ticks per LAPIC_CALIBRATION_US with divisor 16 */

    apic_write(lapic_address, LAPIC_TIMERDIV_REG, 0x3);     /* divisor 16 */
    apic_write(lapic_address, LAPIC_LVTTIMER_REG, 32 | LAPIC_PERIODIC);
    apic_write(lapic_address, LAPIC_INITCNT_REG, calibarion);
}

void init_apic(madt_t *madt){
    asm("cli");
    get_madt_tables(madt);
    
    serial_puts("apic: MADT tables listed through\n");

    /* The legacy 8259 PIC is left in BIOS-default state at boot. With LAPIC
     * LINT0 still in ExtINT mode it will happily deliver legacy IRQs (e.g.
     * IRQ 12 -> mouse) using the BIOS-programmed vector base, which is not
     * present in our IDT. Those interrupts are then silently lost (the IRQ
     * never reaches mouse_handler), which is exactly the failure mode where
     * the desktop cursor draws but never moves. Remap the PIC vectors out of
     * the exception range and mask every line so the IOAPIC is the sole
     * delivery path. */
    pic_remap(0x20, 0x28);
    pic_disable();
    /* IMCR: switch the legacy pair from PIC (virtual wire) to the I/O APIC.
     * QEMU ignores ports 0x22/0x23. Some Hyper-V Gen 1 chipsets keep IRQ1 on
     * the 8259, which we just masked, so the keyboard IRQ never reaches the
     * IOAPIC redirection entry. */
    outb(0x22, 0x70);
    outb(0x23, 0x01);
    serial_puts("apic: legacy 8259 PIC remapped (0x20/0x28) and masked\n");

    serial_puts("apic: writing to SIV register\n");
    hpet_init();
    lapic_addr = (uint32_t*)(uintptr_t)madt->lapicaddr;
    uint32_t spurius_reg = apic_read(lapic_addr, LAPIC_SIVR_REG);
    serial_printf("madt lapic adr: 0x%llx\n", lapic_addr);
    apic_write(lapic_addr, LAPIC_SIVR_REG,spurius_reg | (0x100));

    /* Mask LINT0/LINT1 on the BSP. After reset LINT0 is ExtINT (PIC) and
     * LINT1 is NMI; with the PIC disabled there is nothing useful to deliver
     * via LINT0, and we don't want a stray ExtINT handshake either. */
    apic_write(lapic_addr, LAPIC_LVT_LINT0, 0x10000); /* masked */
    apic_write(lapic_addr, LAPIC_LVT_LINT1, 0x10000); /* masked */
    serial_puts("apic: BSP LINT0/LINT1 masked\n");

    DEBUG_SERIAL("LAPIC initialized\n");
    calibrate_timer(madt);

    /* Keep timer disabled during early boot; kernel_main will call
     * apic_enable_timer() once all subsystems are up.  The calibrated
     * count is already saved in lapic_calibrated_count by calibrate_timer. */
    apic_write(lapic_addr, LAPIC_INITCNT_REG, 0);
    apic_write(lapic_addr, LAPIC_LVTTIMER_REG, 0x10000); /* masked */

    ps2_int_init();
    DEBUG_SERIAL("IOAPIC initialized\n");
    apic_eoi();
}

/* Re-enable the LAPIC periodic timer (vector 32).
 * This is called after all boot initialisation to provide a reliable
 * heartbeat that wakes the CPU from HLT even if HPET is absent or
 * misbehaving (common on Proxmox / bare-metal KVM).                 */
void apic_enable_timer(void) {
    if (!lapic_addr) return;

    /* Use the calibration value from init_apic.
     * calibrate_timer() measures LAPIC ticks over LAPIC_CALIBRATION_US
     * and stores that value for later scaling. */
    uint32_t count = lapic_calibrated_count;
    if (count == 0 || count == 0xFFFFFFFF) {
        serial_printf("apic: WARNING: timer calibration unavailable, skipping LAPIC timer\n");
        return;
    }

    /* The calibrated count is ticks-per-5ms. Scale it down to the
     * scheduler timer period. Nanosecond-class interrupts are not
     * practical here: one context switch already costs far more than
     * a few ns, and a count near 1 can starve the kernel in IRQs. */
    uint64_t scaled_count = ((uint64_t)count * LAPIC_SCHED_PERIOD_US) /
                            LAPIC_CALIBRATION_US;
    uint32_t timer_count = (uint32_t)(scaled_count ? scaled_count : 1);

    apic_write(lapic_addr, LAPIC_TIMERDIV_REG, 0x3);     /* divide by 16    */
    apic_write(lapic_addr, LAPIC_LVTTIMER_REG, 32 | LAPIC_PERIODIC); /* vector 32, periodic, unmasked */
    apic_write(lapic_addr, LAPIC_INITCNT_REG, timer_count);

    serial_printf("apic: LAPIC timer enabled, periodic ~%uus (count=%u)\n",
                  LAPIC_SCHED_PERIOD_US, timer_count);
}

void int_apic_ap(){
    uint32_t spurius_reg = apic_read((uint32_t*)(lapic_addr), 0xF0);
    calibrate_timer(madt);
}