#include "acpi.h"
#include "utils.h"
#include "registers.h"

rsdt_t *rsdt;
xsdt_t *xsdt;
fadt_t *fadt;
madt_t *madt;

#define PMT_TIMER_FREQ 3579545 // 3.579545 MHz

extern uint64_t hhdmoffset; 

static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};


xsdt_t *parse_xsdt(uint64_t hhdmoffset, rsdp_t *rsdp)
{
    if(!rsdp->xsdt_address)
        return NULL;

    return (xsdt_t*)(rsdp->xsdt_address + hhdmoffset);
}

rsdt_t *parse_rsdt(uint64_t hhdmoffset, rsdp_t *rsdp)
{
    if(!rsdp->rsdt_address)
        return NULL;

    return (rsdt_t*)(rsdp->rsdt_address + hhdmoffset);
}

fadt_t *fetch_fadt(){
    return find_acpi_table("FACP",rsdt,xsdt);
}

void *find_acpi_table(char sig[4], rsdt_t *rsdt, xsdt_t *xsdt){
    int usexsdt;
    int entries;
    printf("acpi: looking for table with signature '{cccc}'{n}", sig[0], sig[1], sig[2], sig[3]);
    if(!xsdt){
        usexsdt = 0;
        entries = (rsdt->header.length - sizeof(sdt_t)) / 4;
    }else{
        usexsdt = 1;
        entries = (xsdt->header.length - sizeof(sdt_t)) / 8;
    }
    for(int i = 0; i < entries; i++){
        sdt_t *header;
        if(usexsdt == 0)
            header = (sdt_t*)rsdt->tableptrs[i];
        else
            header = (sdt_t*)xsdt->tableptrs[i];

        if(!memcmp(header->signature + hhdmoffset,sig,4))
        {
            printf("acpi: Found table with signature '{cccc}'{n}", sig[0], sig[1], sig[2], sig[3]);
            return (void*)header;
        }
    }
    printf("acpi: Table '{s}' not found{n}", sig);
    return NULL;
}


void init_acpi(void){
    if(rsdp_request.response == NULL)
        log_panic("RSDP not received, halting");
    
    rsdp_t *rsdp = (rsdp_t*)rsdp_request.response->address;

    rsdt = parse_rsdt(hhdmoffset,rsdp);
    xsdt = parse_xsdt(hhdmoffset,rsdp);

    madt = find_acpi_table("APIC",rsdt,xsdt); // APIC - sig for MADT table

    if(madt == NULL)
        log_panic("MADT table not found");

    fadt = find_acpi_table("FACP",rsdt,xsdt); // FACP - sig for FADT table

    if(fadt == NULL)
        log_panic("FADT table not found");
    
    // Enable ACPI
    init_apic(madt, hhdmoffset);
        
}

void pmt_delay(uint64_t us){
    if(fadt->PMTimerLength != 4)
        log_panic("PMTimer not implemented");
    
    uint64_t count = inl(fadt->PMTimerBlock);
    uint64_t target = count + (us * PMT_TIMER_FREQ) / 1000000;
    uint64_t cur = 0;

    while(cur < target){
        cur = ((inl(fadt->PMTimerBlock) - count) & 0xFFFFFFFF);    
    }
        
}