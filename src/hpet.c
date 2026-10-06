#include "hpet.h"
#include "acpi.h"
#include "utils.h"
#include "apic.h"
#include "cpu.h"
#include "mem.h"
#include "atomic.h"
#include "serial.h"
#include "term.h"
#include "serial.h"
#include <stddef.h>
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

spinlock_t hpet_lock = {0};
uint64_t hpet_freq;
static atomic_t _ticks = {0};
static atomic_t tiks = {0};
static uint64_t current_interval_ms = 0;  // Track current timer interval


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

/* Inserted into hpet.c ahead of hpet_init(). Fallback when the ACPI HPET
 * table is missing (Hyper-V). Prefer the ACPI PM timer; otherwise calibrate
 * the TSC against the PIT and use that as the free-running clock. */

#define CLK_NONE 0
#define CLK_HPET 1
#define CLK_PMT  2
#define CLK_TSC  3

static int clk_src = CLK_NONE;

int hpet_is_hardware(void) { return clk_src == CLK_HPET; }

#define PMT_FREQ 3579545ULL
#define PIT_FREQ 1193182ULL

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t value);
extern uint32_t inl(uint16_t port);

static uint16_t pmt_port;
static uint32_t pmt_mask;
static uint32_t pmt_last;
static uint64_t pmt_accum;

static uint64_t tsc_boot;
static uint64_t tsc_per_ms;

static inline uint64_t hpet_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Extend the wrapping PM counter to 64 bits. Safe from the LAPIC ISR and
 * from thread context: a lost race retries instead of taking a lock. */
static uint64_t pmt_monotonic(void) {
    if (!pmt_port)
        return 0;
    for (int i = 0; i < 16; i++) {
        uint32_t now = inl(pmt_port) & pmt_mask;
        uint32_t last = __atomic_load_n(&pmt_last, __ATOMIC_RELAXED);
        uint32_t delta = (now - last) & pmt_mask;
        if (delta == 0)
            return __atomic_load_n(&pmt_accum, __ATOMIC_RELAXED);
        uint32_t expected = last;
        if (__atomic_compare_exchange_n(&pmt_last, &expected, now,
                false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return __atomic_fetch_add(&pmt_accum, (uint64_t)delta, __ATOMIC_RELAXED) + delta;
        }
    }
    return __atomic_load_n(&pmt_accum, __ATOMIC_RELAXED);
}

static int pmt_port_ticks(uint16_t port, uint32_t mask) {
    uint32_t a = inl(port) & mask;
    for (int i = 0; i < 200000; i++)
        __asm__ volatile("pause");
    return (inl(port) & mask) != a;
}

static uint16_t find_pmt(uint32_t *mask_out) {
    if (!fadt)
        return 0;
    if (fadt->header.length < offsetof(fadt_t, PMTimerBlock) + sizeof(uint32_t))
        return 0;

    uint16_t port = 0;
    if (fadt->PMTimerBlock)
        port = (uint16_t)fadt->PMTimerBlock;

    int have_x = fadt->header.length >= offsetof(fadt_t, X_PMTimerBlock) + sizeof(gas_t);
    if (!port && have_x &&
        fadt->X_PMTimerBlock.AddressSpace == 1 &&
        fadt->X_PMTimerBlock.Address != 0 &&
        fadt->X_PMTimerBlock.Address <= 0xFFFF)
        port = (uint16_t)fadt->X_PMTimerBlock.Address;
    if (!port)
        return 0;

    uint32_t mask = 0x00FFFFFFu;
    if (have_x && fadt->X_PMTimerBlock.BitWidth == 32)
        mask = 0xFFFFFFFFu;
    else if (have_x && fadt->X_PMTimerBlock.BitWidth == 24)
        mask = 0x00FFFFFFu;
    else if (fadt->header.length >= offsetof(fadt_t, Flags) + sizeof(uint32_t) &&
             (fadt->Flags & (1u << 8)))
        mask = 0xFFFFFFFFu;

    if (!pmt_port_ticks(port, mask)) {
        uint32_t alt = (mask == 0xFFFFFFFFu) ? 0x00FFFFFFu : 0xFFFFFFFFu;
        if (!pmt_port_ticks(port, alt))
            return 0;
        mask = alt;
    }
    *mask_out = mask;
    return port;
}

/* PIT channel 2, mode 0, polled via port 0x61 bit 5. No IRQ. */
static int pit_delay_ms(uint32_t ms) {
    uint32_t count = (uint32_t)((PIT_FREQ * ms) / 1000ULL);
    if (count == 0)
        count = 1;
    if (count > 65535)
        count = 65535;

    uint8_t saved = inb(0x61);
    outb(0x61, saved & 0xFC);
    outb(0x43, 0xB0); /* ch2, lo/hi, mode 0, binary */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)((count >> 8) & 0xFF));
    outb(0x61, (uint8_t)((saved & 0xFC) | 0x01));

    uint32_t spins = 0;
    while ((inb(0x61) & 0x20) == 0) {
        __asm__ volatile("pause");
        if (++spins > 100000000u) {
            outb(0x61, (uint8_t)(saved & 0xFC));
            return 0;
        }
    }
    outb(0x61, (uint8_t)(saved & 0xFC));
    /* A stuck-high OUT pin returns immediately; that is not a real delay. */
    return spins >= 1000;
}

static int tsc_calibrate_pit(void) {
    const uint32_t ms = 20;
    uint64_t t0 = hpet_rdtsc();
    if (!pit_delay_ms(ms))
        return 0;
    uint64_t t1 = hpet_rdtsc();
    if (t1 <= t0)
        return 0;
    uint64_t per = (t1 - t0) / ms;
    if (per < 1000ULL || per > 20000000ULL)
        return 0;
    tsc_per_ms = per;
    tsc_boot = t1;
    hpet_freq = per * 1000ULL;
    serial_time_note_tsc_span(t0, t1, (uint64_t)ms * 1000ULL);
    return 1;
}

/* A 32-bit read of a 24-bit PM timer has a noisy top byte, which makes
 * milliseconds jump and the Doom frame cap never wait. Keep a mask only when
 * short samples move by a PM-timer-sized step. */
static int pmt_mask_tracks(uint16_t port, uint32_t mask) {
    uint32_t prev = inl(port) & mask;
    int good = 0, bad = 0;
    for (int i = 0; i < 8; i++) {
        for (int n = 0; n < 250000; n++)
            __asm__ volatile("pause");
        uint32_t now = inl(port) & mask;
        uint32_t d = (now - prev) & mask;
        if (d == 0)
            continue;
        if (d < (uint32_t)(PMT_FREQ / 5))
            good++;
        else
            bad++;
        prev = now;
    }
    return bad == 0 && good > 0;
}

static uint32_t pmt_pit_delta(uint16_t port, uint32_t mask) {
    uint32_t a = inl(port) & mask;
    if (!pit_delay_ms(20))
        return 0;
    uint32_t b = inl(port) & mask;
    return (b - a) & mask;
}

static void clock_fallback_init(void) {
    uint32_t mask = 0;
    uint16_t port = find_pmt(&mask);
    if (port) {
        int ok32 = pmt_mask_tracks(port, 0xFFFFFFFFu);
        int ok24 = pmt_mask_tracks(port, 0x00FFFFFFu);
        if (ok24 && !ok32)
            mask = 0x00FFFFFFu;
        else if (ok32 && !ok24)
            mask = 0xFFFFFFFFu;
        pmt_port = port;
        pmt_mask = mask;
        pmt_last = inl(port) & mask;
        pmt_accum = 0;
        hpet_freq = PMT_FREQ;
        {
            uint32_t d = pmt_pit_delta(port, mask);
            if (d > 1000 && d < 0x10000000u) {
                uint64_t hz = (uint64_t)d * 50ull;
                if (hz >= 100000ull && hz <= 500000000ull)
                    hpet_freq = hz;
            }
        }
        clk_src = CLK_PMT;
        serial_printf("hpet: ACPI PM timer fallback, port=0x%x mask=0x%x freq=%llu ok32=%d ok24=%d\n",
                      (unsigned)port, (unsigned)mask, (unsigned long long)hpet_freq, ok32, ok24);
        uint64_t c0 = pmt_monotonic();
        uint64_t t0 = hpet_rdtsc();
        uint64_t need = hpet_freq / 100ULL; /* 10 ms */
        while ((pmt_monotonic() - c0) < need)
            __asm__ volatile("pause");
        serial_time_note_tsc_span(t0, hpet_rdtsc(), 10000ULL);
        return;
    }

    serial_puts("hpet: ACPI PM timer unavailable, calibrating TSC from PIT\n");
    if (tsc_calibrate_pit()) {
        clk_src = CLK_TSC;
        serial_printf("hpet: TSC fallback, %llu Hz\n", (unsigned long long)hpet_freq);
        return;
    }
    serial_puts("hpet: WARNING: no fallback clock; time will not advance\n");
    clk_src = CLK_NONE;
}

static uint64_t fallback_ms(void) {
    if (clk_src == CLK_PMT) {
        uint64_t hz = hpet_freq ? hpet_freq : PMT_FREQ;
        return pmt_monotonic() * 1000ULL / hz;
    }
    if (clk_src == CLK_TSC && tsc_per_ms)
        return (hpet_rdtsc() - tsc_boot) / tsc_per_ms;
    return atomic_get(&tiks);
}

static uint64_t fallback_us(void) {
    if (clk_src == CLK_PMT) {
        uint64_t hz = hpet_freq ? hpet_freq : PMT_FREQ;
        return pmt_monotonic() * 1000000ULL / hz;
    }
    if (clk_src == CLK_TSC && tsc_per_ms)
        return (hpet_rdtsc() - tsc_boot) * 1000ULL / tsc_per_ms;
    return atomic_get(&tiks) * 1000ULL;
}

void hpet_keepalive(void) {
    if (clk_src == CLK_PMT)
        (void)pmt_monotonic();
}

void hpet_init(){
    if (!hpet || hpet->address.Address == 0) {
        serial_puts("hpet: HPET not found\n");
        clock_fallback_init();
        return;
    }
    serial_puts("hpet: initializing\n");
    serial_puts("hpet: acpi config:\n");
    serial_printf("    hpet comparator count: %d\n", hpet->comparator_count);
    serial_printf("    hpet counter size: %d\n", hpet->counter_size);
    serial_printf("    hpet minimum tick: %d\n", hpet->minimum_tick);
    serial_printf("    hpet legacy replacement: %d\n", hpet->legacy_replacement);

    base_addr = (uint64_t*)(hpet->address.Address + vmm_higher_half_offset);

    hpet_regs = (volatile struct hpet_regs*)base_addr;
    hpet_main_cnt = (volatile uint64_t*)((char*)base_addr + HPET_REG_MAIN_CNT);

    serial_puts("hpet: capabilities:\n");
    uint64_t numTimers = (hpet_regs->capabilities >> 8) & 0x1F;
    serial_printf("    hpet num timers: %d\n", (int)numTimers);
    serial_printf("    hpet legacy route capable: %d\n", (int)((hpet_regs->capabilities >> 15) & 1));
    serial_printf("    hpet 64-bit counter: %d\n", (int)((hpet_regs->capabilities >> 13) & 1));
    uint32_t tick_period  = (uint32_t)(hpet_regs->capabilities >> 32);
    serial_printf("    hpet tick period: %d\n", tick_period);

    hpet_freq = FEMTOSECS_PER_SEC / tick_period;
    serial_printf("    hpet freq: %lld\n", hpet_freq);

    serial_puts("hpet: config:\n");
    uint8_t legacy_replacement_route = (hpet_regs->config >> 1) & 1;
    serial_printf("    hpet legacy replacement route: %d\n", legacy_replacement_route);
    serial_printf("    hpet overall enable: %d\n", (int)(hpet_regs->config & 1));

    /* Calibrate serial timestamps while the counter is running (firmware may
     * leave HPET disabled — a frozen counter would hang calibration). */
    hpet_enable();
    serial_time_calibrate_via_hpet();

    hpet_disable();

    // Reset main counter
    *hpet_main_cnt = 0;

    //configure the main counter
    volatile uint64_t *timer_cfg = _timer_config_reg(0);

    uint32_t routing_caps = (uint32_t)(*timer_cfg >> 32);
    serial_puts("hpet: timer 0 allowed interrupt routing: ");
    serial_printf("0x%08x\n", routing_caps);

    serial_printf("hpet: timer 0 periodic: %d\n", (int)((*timer_cfg >> 4) & 1));
    serial_printf("hpet: timer 0 64-bit: %d\n", (int)((*timer_cfg >> 5) & 1));

    // Clear timer configuration first
    *timer_cfg = 0;
    
    uint8_t selected_ioapic_input = 0;
    *timer_cfg |= (selected_ioapic_input << 9);
    // Don't set periodic mode here - we'll configure it per operation
    *timer_cfg |= (1 << 2); // enable the interrupt
    ioapic_redirect_irq(bsp_lapic_id,(uint8_t)35,(uint8_t)0,true);
    atomic_set(&_ticks, 0);
    atomic_set(&tiks, 0);
    current_interval_ms = 0;
    
    // Don't set up periodic timer during init - let user decide
    clk_src = CLK_HPET;
    serial_puts("hpet: initialization complete\n");
}

void hpet_one_shot(uint64_t ms){
    if (clk_src != CLK_HPET || !hpet_regs) return;
    hpet_reset();
    
    // Get current counter value as starting point
    uint64_t start_count = *hpet_main_cnt;
    
    volatile uint64_t *timer_cfg = _timer_config_reg(0);
    *timer_cfg &= ~(1 << 3); // set to one-shot mode

    volatile uint64_t *timer_comp = _timer_comparator_reg(0);
    uint64_t interval_ticks = (uint64_t)((hpet_freq / 1000) * ms);
    *timer_comp = start_count + interval_ticks;
    
    *timer_cfg |= (1 << 2); // enable the interrupt
    hpet_enable();
    current_interval_ms = 0; // Clear periodic interval for one-shot mode
}

void hpet_set_periodic(uint64_t ms){
    if (clk_src != CLK_HPET || !hpet_regs) {
        current_interval_ms = ms;
        serial_printf("hpet: no HPET, periodic %llums left to LAPIC\n",
                      (unsigned long long)ms);
        return;
    }
    hpet_reset();

    uint64_t interval_ticks = (uint64_t)((hpet_freq / 1000) * ms);

    volatile uint64_t *timer_cfg = _timer_config_reg(0);
    volatile uint64_t *timer_comp = _timer_comparator_reg(0);

    /* Set Tn_TYPE_CNF (bit 3, periodic), Tn_VAL_SET_CNF (bit 6, allows setting
     * the accumulator), and Tn_INT_ENB_CNF (bit 2) in one write.
     * Tn_VAL_SET_CNF MUST be set before writing the comparator so the HPET
     * stores the written value as both the comparator AND the period register.
     * Without bit 6 the period stays 0, QEMU treats the timer as one-shot,
     * and tiks only increments once — making all hpet_get_ticks() timeouts
     * spin forever. */
    *timer_cfg |= (1 << 6) | (1 << 3) | (1 << 2);

    /* Writing interval_ticks (the period) with Tn_VAL_SET_CNF active:
     * QEMU stores cmp=interval_ticks AND period=interval_ticks, then clears
     * bit 6.  QEMU's hpet_set_timer() advances cmp to the next multiple of
     * period that is ahead of the current counter, so the first fire happens
     * within 0..interval_ticks hardware ticks after hpet_enable(). */
    *timer_comp = interval_ticks;

    hpet_enable();
    current_interval_ms = ms;

    serial_printf("hpet: periodic timer set, interval=%llu ticks (%llums)\n",
                  (unsigned long long)interval_ticks, (unsigned long long)ms);
}

inline void hpet_disable(){
    if (!hpet_regs) return;
    hpet_regs->config &= ~1;
}

inline void hpet_enable(){
    if (!hpet_regs) return;
    hpet_regs->config |= 1;
}

void hpet_reset(){
    hpet_disable();
    atomic_set(&_ticks, 0);
}

void hpet_ack(){
    if (!hpet_regs) return;
    /* int_status is write-1-to-clear: writing 1 to bit N clears timer N's
     * pending interrupt.  The old `&= ~1` wrote 0, clearing nothing. */
    hpet_regs->int_status = (uint64_t)1;
}

void hpet_sleep(uint64_t ms){
    if (ms == 0) return;
    if (clk_src != CLK_HPET || !hpet_main_cnt || !hpet_freq) {
        uint64_t start = fallback_ms();
        while ((fallback_ms() - start) < ms)
            __asm__ volatile("pause");
        return;
    }
    /* Busy-wait by reading the HPET main counter directly.
     * The old approach (hpet_one_shot) called hpet_reset() which disabled the
     * HPET and then reprogrammed timer 0 to one-shot mode, silently destroying
     * the periodic timer set up by hpet_set_periodic().  Everything that
     * depends on hpet_get_ticks() (ping inter-ping delay, DNS timeout, etc.)
     * would then spin forever after the first hpet_sleep() call.
     * Reading the main counter is safe, non-destructive, and still allows all
     * hardware interrupts (including NIC interrupts) to fire during the wait. */
    if (ms == 0) return;
    uint64_t ticks_needed = (hpet_freq / 1000) * ms;
    uint64_t start = *hpet_main_cnt;
    while ((*hpet_main_cnt - start) < ticks_needed)
        __asm__ volatile ("pause");
}

void hpet_sleep_counter(uint64_t ms){
    if (clk_src != CLK_HPET || !hpet_main_cnt || !hpet_regs) {
        hpet_sleep(ms);
        return;
    }
    hpet_disable();
    volatile uint64_t *timer_config = _timer_config_reg(0);
    *timer_config &= ~(1 << 2);      // Disable timer 0 interrupts.

    uint64_t start_count = *hpet_main_cnt;
    uint64_t interval_ticks = (uint64_t)((hpet_freq / 1000) * ms);
    uint64_t target_count = start_count + interval_ticks;

    hpet_enable();
    while (*hpet_main_cnt < target_count)
        __asm__ volatile("pause");

    /*
     * Never leave HPET globally disabled: clearing OE freezes MAIN_CNT, so
     * hpet_sleep() (busy-wait on MAIN_CNT, e.g. SDL_Delay from user apps)
     * spins forever. Restore the periodic timer if the task manager / kernel
     * had configured one; otherwise at least re-enable the counter.
     */
    if (current_interval_ms > 0) {
        hpet_set_periodic(current_interval_ms);
    } else {
        hpet_enable();
    }
}

uint64_t hpet_get_ticks(){
    if (clk_src != CLK_HPET || !hpet_main_cnt || !hpet_freq) return fallback_ms();
    /* Read the hardware main counter directly.  This works even when the
     * periodic interrupt stops firing (common after SMP init races).
     * Convert from raw counter ticks to milliseconds. */
    return *hpet_main_cnt / (hpet_freq / 1000);
}

uint64_t hpet_get_time_us(void) {
    if (clk_src != CLK_HPET || !hpet_main_cnt || !hpet_freq) return fallback_us();
    return (*hpet_main_cnt) * 1000000ULL / hpet_freq;
}

uint64_t hpet_get_current_interval(void){
    return current_interval_ms;
}

uint64_t hpet_get_frequency(void){
    return hpet_freq;
}

void hpet_print_status(void){
    if (clk_src != CLK_HPET || !hpet_regs || !hpet_main_cnt) {
        const char *name = "none";
        if (clk_src == CLK_PMT) name = "ACPI PM timer";
        else if (clk_src == CLK_TSC) name = "PIT-calibrated TSC";
        printf("Clock: %s (HPET not present)\n", name);
        printf("  Milliseconds: %llu\n", (unsigned long long)hpet_get_ticks());
        printf("  Frequency: %llu Hz\n", (unsigned long long)hpet_get_frequency());
        return;
    }
    printf("HPET Status:\n");
    printf("  Frequency: %lld Hz\n", hpet_freq);
    printf("  Current interval: %llu ms\n", (unsigned long long)current_interval_ms);
    printf("  Total ticks: %llu\n", (unsigned long long)atomic_get(&tiks));
    printf("  One-shot ticks: %llu\n", (unsigned long long)atomic_get(&_ticks));
    printf("  Main counter: %llu\n", (unsigned long long)*hpet_main_cnt);
    printf("  Timer enabled: %s\n", (hpet_regs->config & 1) ? "yes" : "no");
    
    volatile uint64_t *timer_cfg = _timer_config_reg(0);
    printf("  Timer 0 config: 0x%016llx\n", (unsigned long long)*timer_cfg);
    printf("  Timer 0 periodic: %s\n", (*timer_cfg & (1 << 3)) ? "yes" : "no");
    printf("  Timer 0 enabled: %s\n", (*timer_cfg & (1 << 2)) ? "yes" : "no");
}

void hpet_isr()
{
    atomic_inc(&_ticks);
    atomic_inc(&tiks);

    /* In true periodic mode (bit 3 set) the HPET hardware automatically
     * reloads the comparator.  Writing to it from the ISR interferes with
     * the hardware reload and causes timing glitches on KVM / Proxmox.
     *
     * If the timer is NOT in periodic mode (one-shot), there is nothing
     * to reload -- the next one-shot will be set up by the caller.
     *
     * So: just increment the tick counters and return.  The LAPIC timer
     * serves as a backup heartbeat if HPET periodic mode is broken.     */
}