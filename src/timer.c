#include "timer.h"

uint64_t ticks;
const uint32_t freq = 100;

void onIrq0(registers_t *r) {
    ticks += 1;
    putpixel(ticks, 100, 0xFFFFFF);
}

void initTimer(){
    ticks = 0;
    //irq_install_handler(0, &onIrq0);
    //119318.16666 Mhz
    uint32_t divisor = 1193180 / freq;
    
    //0011 0110
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)divisor & 0xFF);
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}