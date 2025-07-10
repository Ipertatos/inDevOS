#include "timer.h"
#include "atomic.h"

static atomic_t ticks = {0};
const uint32_t freq = 100;

void onIrq0(registers_t *r) {
    atomic_inc(&ticks);
    // Remove putpixel from interrupt context - unsafe!
    // putpixel(ticks, 100, 0xFFFFFF);
}

void initTimer(){
    atomic_set(&ticks, 0);
    //irq_install_handler(0, &onIrq0);
    //119318.16666 Mhz
    uint32_t divisor = 1193180 / freq;
    
    //0011 0110
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)divisor & 0xFF);
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint64_t get_ticks(void) {
    return atomic_get(&ticks);
}