#include "../types.h"
#include "../apic.h"
#include "../hpet.h"
#include "../utils.h"
#include "../cpu.h"
#include "info.h"


void print_info(){
    printf("Uptime: {d} ms{n}", hpet_get_ticks());
    printf("CPU Name: {s}{n}", cpu_name());
}