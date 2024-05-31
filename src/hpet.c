#include "hpet.h"
#include "acpi.h"
#include "utils.h"
#include "apic.h"
#include "cpu.h"
#include "mem.h"
#define FEMTOSECS_PER_SEC 1000000000000000LL

#define HPET_REG_MAIN_CNT           0x0F0
#define HPET_REG_TIMER_CFG          0x100
#define HPET_REG_TIMER_SIZE         0x20
#define HPET_REG_TIMER_COMP         0x108
#define HPET_REG_TIMER_COMP_SIZE    0x20

struct hpet_regs{
    volatile uint64_t capabilities;
    volatile uint64_t reserved1;
    volatile uint64_t config;
    volatile uint64_t reserved2;
    volatile uint64_t int_status;
};

volatile uint64_t *base_addr = 0;
volatile struct hpet_regs *hpet_regs = 0;
volatile uint64_t *hpet_main_cnt = 0;

uint64_t hpet_freq;
volatile uint64_t _ticks;


uint64_t* _timer_config_reg(uint32_t n)
{
    return (uint64_t*)((char*)base_addr + (HPET_REG_TIMER_CFG + (HPET_REG_TIMER_SIZE * n)));
}

uint64_t* _timer_comparator_reg(uint32_t n)
{
    return (uint64_t*)((char*)base_addr + (HPET_REG_TIMER_COMP + (HPET_REG_TIMER_COMP_SIZE * n)));
}

static inline uint64_t hpet_read(uint64_t offset)
{
    return *((volatile uint64_t*)((char*)base_addr + offset));
}

static inline void hpet_write(uint64_t offset, uint64_t val)
{
    *((volatile uint64_t*)((char*)base_addr + offset)) = val;
}

void hpet_init(){
    printf("hpet: initializing{n}");
    printf("hpet: acpi config:{n}");
    printf("    hpet comparator count: {d}{n}", hpet->comparator_count);
    printf("    hpet counter size: {d}{n}", hpet->counter_size);
    printf("    hpet minimum tick: {d}{n}", hpet->minimum_tick);
    printf("    hpet legacy replacement: {d}{n}", hpet->legacy_replacement);

    base_addr = (uint64_t*)(hpet->address.Address + vmm_higher_half_offset);
    printf("hpet: base address: {x}{n}", base_addr);
    printhex(ft_ctx,hpet->address.Address);
    printf("{n}");
    printhex(ft_ctx,vmm_higher_half_offset);
    printf("{n}");



    hpet_regs = (volatile struct hpet_regs*)base_addr;
    hpet_main_cnt = (volatile uint64_t*)((char*)base_addr + HPET_REG_MAIN_CNT);

    printf("hpet: capabilities: {n}");
    uint64_t numTimers = (hpet_regs->capabilities >> 8) & 0x1F;
    printf("    hpet num timers: {d}{n}", numTimers);
    printf("    hpet legacy route capable: {d}{n}", (hpet_regs->capabilities >> 15) & 1);
    printf("    hpet 64-bit counter: {d}{n}", (hpet_regs->capabilities >> 13) & 1);
    uint32_t tick_period  = (uint32_t)(hpet_regs->capabilities >> 32);
    printf("    hpet tick period: {d}{n}", tick_period);

    hpet_freq = FEMTOSECS_PER_SEC / tick_period;
    printf("    hpet freq: {d}{n}", hpet_freq);

    printf("hpet: config: {n}");
    uint8_t legacy_replacement_route = (hpet_regs->config >> 1) & 1;
    printf("    hpet legacy replacement route: {d}{n}", legacy_replacement_route);
    printf("    hpet overall enable: {d}{n}", hpet_regs->config & 1);

    //configure the main counter
    volatile uint64_t *timer_cfg = _timer_config_reg(0);

    uint32_t routing_caps = (uint32_t)(*timer_cfg >> 32);
    printf("hpet: timer 0 allowed interrupt routing: ");
    printbin(ft_ctx,routing_caps);
    printf("{n}");    

    printf("hpet: timer 0 periodic: {d}{n}", (*timer_cfg >> 4) & 1);
    printf("hpet: timer 0 64-bit: {d}{n}", (*timer_cfg >> 5) & 1);

    uint8_t selected_ioapic_input = 0;
    *timer_cfg |= (selected_ioapic_input << 9);
    *timer_cfg |= (1 << 3); 
    *timer_cfg |= (1 << 2); // enable the interrupt
    ioapic_redirect_irq(bsp_lapic_id,(uint8_t)32,(uint8_t)0,true);
    _ticks = 0;
}