#include "gdt.h"
#include "utils.h"

#define GDTSEGMENTS 7 // 6 + 1 - TSS takes up 2

static gdt_entry_t gdt[GDTSEGMENTS];

gdt_pointer_t gdt_pointer;
tss_t tss;

void gdt_set_entry(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t granularity){
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    gdt[num].granularity |= (granularity & 0xF) << 4;
    gdt[num].access = access;
}

void gdt_init(){
    gdt_pointer.size = (sizeof(gdt_pointer) * GDTSEGMENTS) - 1;
    gdt_pointer.offset = (uint64_t)&gdt;

    memset(&tss , 0 , sizeof(tss));

    //entry 0 null
    gdt_set_entry(0,0,0,0,0);

    //entry 1 kmode code seg
    gdt_set_entry(1,0,0xFFFF, 0x9A, 0xA);

    //entry 2 kmode data seg
    gdt_set_entry(2,0,0xFFFF, 0x92, 0xA);

    //entry 3 umode code seg
    gdt_set_entry(3,0,0xFFFF, 0xFA, 0xA);

    //entry 4 umode data seg
    gdt_set_entry(4,0,0xFFFF,0xF2,0xA);

    //entry 5 TSS
    gdt_set_entry(5,(uint64_t)&tss,sizeof(tss),0x89,0x0);

    s_setgdt(gdt_pointer.size, gdt_pointer.offset);

    s_settss();
}