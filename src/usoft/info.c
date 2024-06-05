#include "../types.h"
#include "../apic.h"
#include "info.h"


uint64_t getUptime(){
    return apic_timer_ticks;
}

void print_info(){
    printf("Uptime: {d} ms{n}", getUptime());
    apic_asleep(10);
    printf("Uptime: {d} ms{n}", getUptime());
}