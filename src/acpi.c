#include "acpi.h"
#include "utils.h"
#include "registers.h"
#include "mem.h"
#include "apic.h"
#include "term.h"
#include "serial.h"
#include "smp.h"
#include "hpet.h"
rsdt_t *rsdt;
xsdt_t *xsdt;
fadt_t *fadt;
madt_t *madt;
hpet_t *hpet;
mcfg_entry *mcfg;

#define PMT_TIMER_FREQ 3579545 // 3.579545 MHz

/* ============================================================================
 * ACPI Power Management
 * ============================================================================ */

/** @brief SLP_EN bit in PM1 control register */
#define ACPI_SLP_EN     (1 << 13)

/** @brief SLP_TYP bit offset in PM1 control register */
#define ACPI_SLP_TYP_SHIFT  10

/** @brief Parsed S5 sleep type values from DSDT */
static uint16_t slp_typa = 0;
static uint16_t slp_typb = 0;
static bool s5_parsed = false;

/**
 * @brief Parse the DSDT table to find \_S5_ sleep type values
 *
 * The DSDT contains AML bytecode with a \_S5_ package that specifies
 * the SLP_TYPa and SLP_TYPb values needed for ACPI S5 (soft off) state.
 * We search for the "_S5_" signature and parse the following AML package.
 *
 * AML encoding:
 *   NameOp (0x08) + "_S5_" + PackageOp (0x12) + PkgLength + NumElements
 *   + BytePrefix (0x0A) value | ZeroOp (0x00) | OneOp (0x01)
 */
static void parse_s5_from_dsdt(void) {
    /* Get DSDT address from FADT (prefer 64-bit X_Dsdt) */
    /* X_Dsdt only exists in ACPI 2.0+ FADTs. An ACPI 1.0 FADT (QEMU -M pc /
     * SeaBIOS, length 116) ends before it, so reading it returns bytes of the
     * next table and the HHDM pointer can be non-canonical (#GP at boot). */
    uint64_t dsdt_addr = (uint64_t)fadt->Dsdt;
    if (fadt->header.length >= __builtin_offsetof(fadt_t, X_Dsdt) + sizeof(uint64_t) &&
        fadt->X_Dsdt && fadt->X_Dsdt < 0x0000800000000000ULL)
        dsdt_addr = fadt->X_Dsdt;
    if (!dsdt_addr) {
        serial_puts("acpi: no DSDT address in FADT\n");
        return;
    }

    sdt_t *dsdt = (sdt_t *)(dsdt_addr + vmm_higher_half_offset);
    uint32_t length = dsdt->length;
    uint8_t *ptr = (uint8_t *)dsdt;

    serial_printf("acpi: searching DSDT at 0x%llx (length %d) for \\_S5_\n",
                  (unsigned long long)dsdt_addr, length);

    /* Search for "_S5_" in the DSDT bytecode */
    for (uint32_t i = 0; i < length - 8; i++) {
        if (ptr[i] == '_' && ptr[i+1] == 'S' && ptr[i+2] == '5' && ptr[i+3] == '_') {
            /* Verify NameOp (0x08) precedes the name */
            if (i == 0 || (ptr[i-1] != 0x08 && ptr[i-2] != 0x08)) {
                continue;  /* Not a Name declaration, keep searching */
            }

            uint8_t *s5 = ptr + i + 4;

            /* Expect PackageOp (0x12) */
            if (*s5 != 0x12) {
                serial_printf("acpi: found _S5_ but no PackageOp (got 0x%02x)\n", *s5);
                continue;
            }
            s5++;

            /* Parse PkgLength (variable-length encoding) */
            uint8_t pkg_lead = *s5;
            uint8_t byte_count = (pkg_lead >> 6) & 3;
            if (byte_count == 0) {
                s5++;  /* Single-byte length */
            } else {
                s5 += byte_count + 1;  /* Multi-byte length */
            }

            /* Skip NumElements byte */
            s5++;

            /* Parse SLP_TYPa */
            if (*s5 == 0x0A) {          /* BytePrefix */
                s5++;
                slp_typa = *s5++;
            } else if (*s5 == 0x00) {   /* ZeroOp */
                slp_typa = 0;
                s5++;
            } else if (*s5 == 0x01) {   /* OneOp */
                slp_typa = 1;
                s5++;
            } else {
                serial_printf("acpi: unexpected AML encoding for SLP_TYPa: 0x%02x\n", *s5);
                slp_typa = 0;
                s5++;
            }

            /* Parse SLP_TYPb */
            if (*s5 == 0x0A) {
                s5++;
                slp_typb = *s5;
            } else if (*s5 == 0x00) {
                slp_typb = 0;
            } else if (*s5 == 0x01) {
                slp_typb = 1;
            } else {
                slp_typb = 0;
            }

            s5_parsed = true;
            serial_printf("acpi: parsed \\_S5_ - SLP_TYPa=%d, SLP_TYPb=%d\n",
                          slp_typa, slp_typb);
            return;
        }
    }

    serial_puts("acpi: \\_S5_ not found in DSDT\n");
}

/**
 * @brief Enable ACPI mode if not already enabled
 *
 * On legacy systems, we need to write AcpiEnable to SMI_CommandPort
 * to transition from legacy mode to ACPI mode.
 */
static void acpi_enable_mode(void) {
    /* Check if ACPI is already enabled via PM1a control block */
    if (fadt->PM1aControlBlock) {
        uint16_t pm1a = inw(fadt->PM1aControlBlock);
        if (pm1a & 1) {
            /* SCI_EN bit is set - ACPI already enabled */
            return;
        }
    }

    /* Enable ACPI via SMI command port */
    if (fadt->SMI_CommandPort && fadt->AcpiEnable) {
        serial_printf("acpi: enabling ACPI mode (SMI port 0x%x, enable cmd 0x%x)\n",
                      fadt->SMI_CommandPort, fadt->AcpiEnable);
        outb(fadt->SMI_CommandPort, fadt->AcpiEnable);

        /* Wait for SCI_EN to be set */
        for (int i = 0; i < 300; i++) {
            if (inw(fadt->PM1aControlBlock) & 1) {
                serial_puts("acpi: ACPI mode enabled\n");
                return;
            }
            pmt_delay(10000);  /* 10ms */
        }
        serial_puts("acpi: warning - ACPI enable timed out\n");
    }
}

void acpi_power_init(void) {
    if (!fadt) return;
    parse_s5_from_dsdt();
    acpi_enable_mode();
}

bool acpi_s5_available(void) {
    return s5_parsed;
}

void acpi_shutdown(void) {
    if (!fadt) {
        printf("ACPI: No FADT table - cannot shut down\n");
        return;
    }

    /* Parse S5 if not done yet */
    if (!s5_parsed) {
        parse_s5_from_dsdt();
    }

    printf("ACPI: Initiating system shutdown...\n");
    serial_puts("acpi: *** SYSTEM SHUTDOWN ***\n");

    /* Halt all other CPUs first */
    smp_halt_all_except_self();

    /* Disable interrupts */
    __asm__ volatile("cli");

    /* Write SLP_TYPa | SLP_EN to PM1a control block */
    if (fadt->PM1aControlBlock) {
        uint16_t val = (slp_typa << ACPI_SLP_TYP_SHIFT) | ACPI_SLP_EN;
        serial_printf("acpi: writing 0x%04x to PM1a_CNT (port 0x%x)\n",
                      val, fadt->PM1aControlBlock);
        outw(fadt->PM1aControlBlock, val);
    }

    /* Write SLP_TYPb | SLP_EN to PM1b control block (if present) */
    if (fadt->PM1bControlBlock) {
        uint16_t val = (slp_typb << ACPI_SLP_TYP_SHIFT) | ACPI_SLP_EN;
        serial_printf("acpi: writing 0x%04x to PM1b_CNT (port 0x%x)\n",
                      val, fadt->PM1bControlBlock);
        outw(fadt->PM1bControlBlock, val);
    }

    /* If standard ACPI shutdown failed, try QEMU-specific port */
    outw(0x604, 0x2000);

    /* Last resort: halt */
    printf("ACPI: Shutdown failed - system halted\n");
    serial_puts("acpi: shutdown failed, halting\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}

void acpi_reboot(void) {
    if (!fadt) {
        printf("ACPI: No FADT table - using fallback reboot\n");
        goto fallback;
    }

    printf("ACPI: Initiating system reboot...\n");
    serial_puts("acpi: *** SYSTEM REBOOT ***\n");

    /* Halt all other CPUs first */
    smp_halt_all_except_self();

    /* Disable interrupts */
    __asm__ volatile("cli");

    /* Method 1: ACPI ResetReg from FADT */
    if (fadt->ResetReg.Address) {
        serial_printf("acpi: using FADT ResetReg (space=%d, addr=0x%llx, value=0x%02x)\n",
                      fadt->ResetReg.AddressSpace,
                      (unsigned long long)fadt->ResetReg.Address,
                      fadt->ResetValue);

        switch (fadt->ResetReg.AddressSpace) {
        case 0:  /* Memory space */
            *(volatile uint8_t *)(fadt->ResetReg.Address + vmm_higher_half_offset)
                = fadt->ResetValue;
            break;
        case 1:  /* I/O space */
            outb((uint16_t)fadt->ResetReg.Address, fadt->ResetValue);
            break;
        case 2:  /* PCI config space */
            /* PCI config space reboot not commonly used */
            break;
        }

        /* Wait a bit for the reset to take effect */
        for (volatile int i = 0; i < 100000; i++) {
            __asm__ volatile("nop");
        }
    }

fallback:
    serial_puts("acpi: FADT reset failed, trying 8042 keyboard controller\n");

    /* Method 2: 8042 keyboard controller reset */
    {
        uint8_t status;
        do {
            status = inb(0x64);
            if (status & 0x01) inb(0x60);  /* Drain output buffer */
        } while (status & 0x02);
        outb(0x64, 0xFE);  /* System reset command */
    }

    /* Wait for reset */
    for (volatile int i = 0; i < 100000; i++) {
        __asm__ volatile("nop");
    }

    serial_puts("acpi: 8042 reset failed, trying triple fault\n");

    /* Method 3: Triple fault - load null IDT and trigger interrupt */
    {
        struct {
            uint16_t limit;
            uint64_t base;
        } __attribute__((packed)) null_idt = {0, 0};
        __asm__ volatile("lidt %0" : : "m"(null_idt));
        __asm__ volatile("int $3");
    }

    /* Should never reach here */
    while (1) {
        __asm__ volatile("hlt");
    }
}

static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};


xsdt_t *parse_xsdt(uint64_t vmm_higher_half_offset, rsdp_t *rsdp)
{
    if(!rsdp->xsdt_address)
        return NULL;

    return (xsdt_t*)((uint64_t)rsdp->xsdt_address + vmm_higher_half_offset);
}

rsdt_t *parse_rsdt(uint64_t vmm_higher_half_offset, rsdp_t *rsdp)
{
    if(!rsdp->rsdt_address)
        return NULL;

    return (rsdt_t*)((uint64_t)rsdp->rsdt_address + vmm_higher_half_offset);
}

fadt_t *fetch_fadt(){
    return find_acpi_table("FACP",rsdt,xsdt);
}

mcfg_entry *fetch_mcfg(){
    return find_acpi_table("MCFG",rsdt,xsdt);
}

void *find_acpi_table(char sig[4], rsdt_t *rsdt, xsdt_t *xsdt){
    int usexsdt;
    int entries;
    printf("acpi: looking for table with signature '%c%c%c%c'\n", sig[0], sig[1], sig[2], sig[3]);
    if(!xsdt){
        usexsdt = 0;
        entries = (rsdt->header.length - 36) / 4;
    }else{
        usexsdt = 1;
        entries = (xsdt->header.length - 36) / 8;
    }
    for(int i = 0; i < entries; i++){
        sdt_t *header;
        if(usexsdt == 0)
            header = (sdt_t*)((uint64_t)rsdt->tableptrs[i] + vmm_higher_half_offset);
        else
            header = (sdt_t*)((uint64_t)xsdt->tableptrs[i] + vmm_higher_half_offset);

        if(!memcmp(header->signature,sig,4))
        {
            printf("acpi: Found table with signature '%c%c%c%c'\n", sig[0], sig[1], sig[2], sig[3]);
            return (void*)header;
        }
    }
    printf("acpi: Table '%s' not found\n", sig);
    return NULL;
}


void init_acpi(void){
    if(rsdp_request.response == NULL)
        log_panic("RSDP not received, halting");
    
    rsdp_t *rsdp = (rsdp_t*)rsdp_request.response->address;

    rsdt = parse_rsdt(vmm_higher_half_offset,rsdp);
    xsdt = parse_xsdt(vmm_higher_half_offset,rsdp);

    madt = find_acpi_table("APIC",rsdt,xsdt); // APIC - sig for MADT table

    if(madt == NULL)
        log_panic("MADT table not found");

    fadt = find_acpi_table("FACP",rsdt,xsdt); // FACP - sig for FADT table

    if(fadt == NULL)
        log_panic("FADT table not found");
    
    hpet = find_acpi_table("HPET",rsdt,xsdt); // HPET - sig for HPET table

    /* Hyper-V and some other VMs omit the HPET table. Do not halt: hpet_init()
     * selects the ACPI PM timer (or a PIT-calibrated TSC) so ktime still moves. */
    if(hpet == NULL) {
        log_warn("HPET table not found, using fallback clock");
        serial_printf("acpi: HPET table not found, using fallback clock\n");
    }

    mcfg = find_acpi_table("MCFG",rsdt,xsdt); // MCFG - sig for MCFG table

    /* Hyper-V and some other VMs omit the MCFG table (no ECAM). Do not halt:
     * pci_device_read/write fall back to legacy port 0xCF8/0xCFC config. */
    if(mcfg == NULL) {
        log_warn("MCFG table not found, using legacy PCI config");
        serial_printf("acpi: MCFG table not found, using legacy PCI config (0xCF8/0xCFC)\n");
    }

    // Enable ACPI
    init_apic(madt);
        
}

static void acpi_delay_fallback_us(uint64_t us) {
    if (us >= 1000)
        hpet_sleep(us / 1000);
    else {
        uint64_t spins = us * 500;
        while (spins--)
            __asm__ volatile("pause");
    }
}

void pmt_delay(uint64_t us){
    if (!fadt || fadt->PMTimerBlock == 0 || fadt->PMTimerLength == 0) {
        acpi_delay_fallback_us(us);
        return;
    }

    /* ACPI PM timer: 3.579545 MHz. PMTimerLength is in bits (24 or 32 typical). */
    uint32_t mask = 0xFFFFFFFFu;
    if (fadt->PMTimerLength == 1)
        mask = 0xFFu;
    else if (fadt->PMTimerLength == 2)
        mask = 0xFFFFu;
    else if (fadt->PMTimerLength == 3)
        mask = 0xFFFFFFu;
    else if (fadt->PMTimerLength != 4) {
        serial_printf("ACPI: unsupported PMTimerLength=%u, using fallback delay\n",
                      (unsigned)fadt->PMTimerLength);
        acpi_delay_fallback_us(us);
        return;
    }

    uint64_t needed = (us * PMT_TIMER_FREQ) / 1000000;
    if (needed == 0)
        return;

    uint64_t start = inl(fadt->PMTimerBlock);
    uint64_t elapsed = 0;

    while (elapsed < needed) {
        elapsed = (inl(fadt->PMTimerBlock) - start) & mask;
    }
}