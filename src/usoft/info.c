#include "../types.h"
#include "../apic.h"
#include "../hpet.h"
#include "../utils.h"
#include "../cpu.h"
#include "info.h"


void print_info(){
    printf("OS Name: inDevOS{n}");
    printf("OS Version: 0.0.1{n}");
    printf("CPU Name: {s}{n}", cpu_name());
    printf("Uptime: {d} ticks{n}", hpet_get_ticks());
}