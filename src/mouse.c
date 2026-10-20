#include "mouse.h"
#include "utils.h"
#include "keyboard.h"
#ifdef ENABLE_LUA
#include "luade_input.h"
#endif
#include "serial.h"
#include "apic.h"
#include "atomic.h"
#include "cpuid.h"
#include <stdint.h>
#include <stdbool.h>

/* PS/2 Mouse Driver for inDevOS */

// Mouse state
static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

/* PS/2 packet deltas accumulated for user apps (cleared by syscall). */
volatile int sdl_mouse_accum_dx = 0;
volatile int sdl_mouse_accum_dy = 0;
static bool mouse_initialized = false;
static spinlock_t mouse_lock = {0};

// Mouse packet parsing
static uint8_t mouse_packet[4];       // 4 bytes for IntelliMouse scroll wheel
static int mouse_packet_pos = 0;

// IntelliMouse (scroll wheel) support
static bool intellimouse_enabled = false;
static int mouse_packet_size = 3;     // 3 for standard, 4 for IntelliMouse
static volatile int8_t scroll_delta_accum = 0; // Accumulated scroll delta

// Screen bounds (set by mouse_init)
static int screen_width = 1024;
static int screen_height = 768;

/* PS/2 spec: +Y is up. Screen Y grows down, so the usual fix is to negate dy.
 * Hyper-V's emulated i8042 mouse already reports +Y as down, so negating
 * flips the cursor. QEMU and real hardware keep the spec sign. */
static bool ps2_y_positive_is_down = false;

static bool mouse_host_is_hyperv(void) {
    uint32_t regs[4];
    cpuid_string(1, regs);
    if ((regs[2] & 0x80000000u) == 0)
        return false;
    cpuid_string(0x40000000u, regs);
    /* EBX, ECX, EDX spell Microsoft Hv */
    return regs[1] == 0x7263694Du && regs[2] == 0x666F736Fu && regs[3] == 0x76482074u;
}

// PS/2 mouse commands
#define MOUSE_CMD_RESET           0xFF
#define MOUSE_CMD_RESEND          0xFE  
#define MOUSE_CMD_SET_DEFAULTS    0xF6
#define MOUSE_CMD_DISABLE         0xF5
#define MOUSE_CMD_ENABLE          0xF4
#define MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define MOUSE_CMD_GET_DEVICE_ID   0xF2
#define MOUSE_CMD_SET_RESOLUTION  0xE8

// PS/2 controller commands
#define PS2_CMD_READ_CONFIG       0x20
#define PS2_CMD_WRITE_CONFIG      0x60
#define PS2_CMD_DISABLE_AUX       0xA7
#define PS2_CMD_ENABLE_AUX        0xA8
#define PS2_CMD_TEST_AUX          0xA9
#define PS2_CMD_SEND_TO_AUX       0xD4

// Status register bits
#define PS2_STATUS_OUTPUT_FULL    0x01
#define PS2_STATUS_INPUT_FULL     0x02
#define PS2_STATUS_SYSTEM_FLAG    0x04
#define PS2_STATUS_COMMAND_DATA   0x08
#define PS2_STATUS_TIMEOUT        0x40
#define PS2_STATUS_PARITY_ERROR   0x80

// Wait for PS/2 controller input buffer to be empty
static void mouse_wait_input(void) {
    int timeout = 100000;
    while ((inb(0x64) & PS2_STATUS_INPUT_FULL) && --timeout > 0) {
        // Wait
    }
}

// Wait for PS/2 controller output buffer to have data
static void mouse_wait_output(void) {
    int timeout = 100000;  
    while (!(inb(0x64) & PS2_STATUS_OUTPUT_FULL) && --timeout > 0) {
        // Wait
    }
}

// Send command to PS/2 mouse
static void mouse_send_command(uint8_t cmd) {
    mouse_wait_input();
    outb(0x64, PS2_CMD_SEND_TO_AUX);
    mouse_wait_input();
    outb(0x60, cmd);
}

// Read response from PS/2 mouse
static uint8_t mouse_read_response(void) {
    mouse_wait_output();
    return inb(0x60);
}

// Initialize PS/2 mouse
bool mouse_init(void) {
    printf("PS/2 Mouse: Starting initialization\n");

    mouse_sync_screen_bounds();
    ps2_y_positive_is_down = mouse_host_is_hyperv();
    printf("PS/2 Mouse: Screen bounds: %dx%d\n", screen_width, screen_height);
    printf("PS/2 Mouse: Y sign %s\n",
           ps2_y_positive_is_down ? "Hyper-V, positive is down" : "standard, positive is up");

    /* Start mouse at center of screen */
    mouse_x = screen_width / 2;
    mouse_y = screen_height / 2;
    
    // Enable auxiliary (mouse) port
    mouse_wait_input();
    outb(0x64, PS2_CMD_ENABLE_AUX);
    
    /* Test auxiliary port. A timeout must not read 0x60: an empty data
     * port reads as 0xFF on Hyper-V and would be treated as a failed test
     * only after stealing a keyboard byte, or as 0x00 (pass) if we are
     * unlucky. No response means there is no AUX device. */
    mouse_wait_input();
    outb(0x64, PS2_CMD_TEST_AUX);
    {
        int timeout = 100000;
        while (!(inb(0x64) & PS2_STATUS_OUTPUT_FULL) && --timeout > 0) { }
        if (timeout <= 0) {
            printf("PS/2 Mouse: Auxiliary port test timed out\n");
            return false;
        }
    }
    uint8_t test_result = inb(0x60);
    if (test_result != 0x00) {
        printf("PS/2 Mouse: Auxiliary port test failed: 0x%02x\n", test_result);
        return false;
    }
    
    /* Enable mouse interrupts without disabling the keyboard. A timed-out
     * or 0xFF config read must not be written back: bit 4 set turns the
     * keyboard clock off (Hyper-V then delivers no keystrokes). */
    mouse_wait_input();
    outb(0x64, PS2_CMD_READ_CONFIG);
    {
        int timeout = 100000;
        while (!(inb(0x64) & PS2_STATUS_OUTPUT_FULL) && --timeout > 0) { }
        if (timeout <= 0) {
            printf("PS/2 Mouse: config read timed out, leaving keyboard config\n");
            return false;
        }
    }
    uint8_t config = inb(0x60);
    if (config == 0xFF) {
        printf("PS/2 Mouse: config read 0xFF, not writing\n");
        return false;
    }
    printf("PS/2 Mouse: controller config before write: 0x%02x\n", config);
    config |= 0x02;  /* AUX interrupt */
    config |= 0x01;  /* keyboard interrupt */
    config |= 0x40;  /* scancode translation (set 2 -> set 1) */
    config &= (uint8_t)~0x20; /* AUX clock enabled */
    config &= (uint8_t)~0x10; /* keyboard clock enabled */
    mouse_wait_input();
    outb(0x64, PS2_CMD_WRITE_CONFIG);
    mouse_wait_input();
    outb(0x60, config);
    printf("PS/2 Mouse: controller config after  write: 0x%02x (bit1=AUX_IRQ, bit5=AUX_DIS)\n", config);

    /* Verify the controller actually accepted our config */
    mouse_wait_input();
    outb(0x64, PS2_CMD_READ_CONFIG);
    mouse_wait_output();
    uint8_t verify = inb(0x60);
    printf("PS/2 Mouse: controller config readback:    0x%02x\n", verify);
    if ((verify & 0x02) == 0) {
        printf("PS/2 Mouse: WARNING - AUX interrupt enable bit NOT set in controller!\n");
    }
    
    // Reset mouse
    mouse_send_command(MOUSE_CMD_RESET);
    uint8_t ack = mouse_read_response();
    if (ack != 0xFA) {
        printf("PS/2 Mouse: Reset failed, got 0x%02x instead of 0xFA\n", ack);
        return false;
    }
    
    // Wait for BAT completion (0xAA) and device ID (0x00)
    uint8_t bat = mouse_read_response();
    uint8_t id = mouse_read_response();
    printf("PS/2 Mouse: BAT result: 0x%02x, Device ID: 0x%02x\n", bat, id);
    
    // Set default settings
    mouse_send_command(MOUSE_CMD_SET_DEFAULTS);
    ack = mouse_read_response();
    if (ack != 0xFA) {
        printf("PS/2 Mouse: Set defaults failed\n");
        return false;
    }
    
    // Enable mouse
    mouse_send_command(MOUSE_CMD_ENABLE);
    ack = mouse_read_response();
    if (ack != 0xFA) {
        printf("PS/2 Mouse: Enable failed\n");
        return false;
    }

    // Try to enable IntelliMouse scroll wheel protocol.
    // Magic sequence: set sample rate 200, then 100, then 80, then read device ID.
    // If device ID changes from 0x00 to 0x03, scroll wheel is active.
    mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE);
    mouse_read_response();
    mouse_send_command(200);
    mouse_read_response();

    mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE);
    mouse_read_response();
    mouse_send_command(100);
    mouse_read_response();

    mouse_send_command(MOUSE_CMD_SET_SAMPLE_RATE);
    mouse_read_response();
    mouse_send_command(80);
    mouse_read_response();

    // Read device ID
    mouse_send_command(MOUSE_CMD_GET_DEVICE_ID);
    ack = mouse_read_response();
    if (ack == 0xFA) {
        uint8_t new_id = mouse_read_response();
        if (new_id == 0x03) {
            intellimouse_enabled = true;
            mouse_packet_size = 4;
            printf("PS/2 Mouse: IntelliMouse scroll wheel enabled (ID=0x03)\n");
        } else {
            printf("PS/2 Mouse: Standard mouse (ID=0x%02x), no scroll wheel\n", new_id);
        }
    }

    // Re-enable mouse after the ID probe (some controllers disable on ID read)
    mouse_send_command(MOUSE_CMD_ENABLE);
    mouse_read_response();
    
    // Set up interrupt for IRQ 12 (mouse)
    printf("PS/2 Mouse: Configuring IOAPIC redirect (Vector: 45, IRQ: 12)\n");
    extern uint32_t bsp_lapic_id;
    extern void ioapic_redirect_irq(uint32_t lapic_id, uint8_t vector, uint8_t irq, bool enable);
    ioapic_redirect_irq(bsp_lapic_id, 45, 12, true);
    
    mouse_initialized = true;
    printf("PS/2 Mouse: Initialization complete (packet size: %d bytes)\n", mouse_packet_size);
    return true;
}

// Process a complete mouse packet
static void mouse_process_packet(void) {
    uint8_t status = mouse_packet[0];
    int8_t dx = (int8_t)mouse_packet[1];
    int8_t dy = (int8_t)mouse_packet[2];

    /* Diagnostic logging is OFF by default — at 115200 baud each ~150-byte
     * line costs ~13 ms inside the IRQ handler, which destroyed user-app
     * frame rates (DOOM dropped to ~3 fps). Re-enable by defining
     * MOUSE_DEBUG_PACKETS at build time if you ever need to debug PS/2
     * packet alignment again. */
    static volatile uint32_t pkt_count = 0;
    pkt_count++;
#ifdef MOUSE_DEBUG_PACKETS
    bool log_this = (pkt_count <= 8) || ((pkt_count & 0x1F) == 0);
    if (log_this) {
        serial_printf("[mouse] PKT #%u bytes=%02x %02x %02x %02x  hdr_bit3=%d "
                      "dx=%d dy=%d  pos_before=(%d,%d)\n",
                      pkt_count,
                      mouse_packet[0], mouse_packet[1],
                      mouse_packet[2], mouse_packet[3],
                      (status >> 3) & 1,
                      (int)dx, (int)dy,
                      mouse_x, mouse_y);
    }
#else
    (void)pkt_count;
#endif

    /* Movement overflow: 8-bit delta is invalid; ignore to avoid jumps / desync. */
    if (status & 0xC0) {
        dx = 0;
        dy = 0;
    }
    
    // Extract scroll wheel delta (4th byte, only for IntelliMouse)
    int8_t scroll = 0;
    if (intellimouse_enabled && mouse_packet_size >= 4) {
        scroll = (int8_t)mouse_packet[3];
        if (scroll != 0) {
            scroll_delta_accum += scroll;
        }
    }

    // Extract button states
    bool left_button = (status & 0x01) != 0;
    bool right_button = (status & 0x02) != 0; 
    bool middle_button = (status & 0x04) != 0;
    
    // Calculate new position
    int y_screen = ps2_y_positive_is_down ? (int)dy : -(int)dy;
    int new_x = mouse_x + dx;
    int new_y = mouse_y + y_screen;

    // Apply bounds checking
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x >= screen_width) new_x = screen_width - 1;
    if (new_y >= screen_height) new_y = screen_height - 1;

    // Detect button state changes
    uint8_t new_buttons = 0;
    if (left_button) new_buttons |= 0x01;
    if (right_button) new_buttons |= 0x02;
    if (middle_button) new_buttons |= 0x04;

    // Generate move event if position changed
    if (dx != 0 || dy != 0) {
        /* Match SDL_MOUSEMOTION yrel: screen delta uses new_y - old_y = -(packet dy) for pure Y move. */
        sdl_mouse_accum_dx += (int)dx;
        sdl_mouse_accum_dy += y_screen;

        // Update mouse position
        mouse_x = new_x;
        mouse_y = new_y;

#ifdef MOUSE_DEBUG_PACKETS
        if (log_this) {
            serial_printf("[mouse] PKT #%u pos_after=(%d,%d)  screen=%dx%d\n",
                          pkt_count, mouse_x, mouse_y,
                          screen_width, screen_height);
        }
#endif

        // Generate LuaDE event (keep for compatibility)
#ifdef ENABLE_LUA
        luade_mouse_event(mouse_x, mouse_y, 0, false);
#endif
    }
    
    // Generate button events for changes
    for (int button = 1; button <= 3; button++) {
        uint8_t mask = 1 << (button - 1);
        bool was_pressed = (mouse_buttons & mask) != 0;
        bool is_pressed = (new_buttons & mask) != 0;

        if (was_pressed != is_pressed) {
#ifdef ENABLE_LUA
            luade_mouse_event(mouse_x, mouse_y, button, is_pressed);
#endif

        }
    }

    // Handle scroll wheel events
    (void)scroll;
    
    mouse_buttons = new_buttons;
}

// Mouse interrupt handler (called from IDT)
void mouse_handler(void) {
    /* One-shot diagnostic: confirm the IRQ12 path actually fires.
     * Periodic IRQ-count logging removed: with 200 Hz mouse it cost ~5 ms
     * every 1.3 s blocked on the UART, choking SDL apps. */
    static volatile bool mouse_irq_logged = false;
    if (!mouse_irq_logged) {
        mouse_irq_logged = true;
        serial_puts("[mouse] IRQ12 first delivery — handler reached\n");
    }

    if (!mouse_initialized) {
        apic_eoi();
        return;
    }

    /* Already in interrupt context: interrupts are disabled by the IDT gate.
     * A plain spinlock is sufficient here; the readers below use irq_save so
     * we cannot be re-entered on the same CPU mid-critical-section. */
    spinlock_lock(&mouse_lock);
    
    // Read data from mouse
    uint8_t data = inb(0x60);

    /* PS/2 mouse: first byte of each packet always has bit 3 set (OSDev).  If we
     * start mid-packet after noise or a dropped byte, dx/dy misalign — often Y
     * sticks while X still changes.  Drop bytes until we see a valid start. */
    if (mouse_packet_pos == 0 && (data & 0x08) == 0) {
        spinlock_unlock(&mouse_lock);
        apic_eoi();
        return;
    }

    // Build packet (3 or 4 bytes depending on IntelliMouse mode)
    mouse_packet[mouse_packet_pos] = data;
    mouse_packet_pos++;
    
    if (mouse_packet_pos >= mouse_packet_size) {
        // Complete packet received
        mouse_process_packet();
        mouse_packet_pos = 0;
    }
    
    spinlock_unlock(&mouse_lock);
    apic_eoi();
}

/* IRQ-safe lock helpers — the readers run from the main loop and the IRQ12
 * handler also takes mouse_lock. Without disabling interrupts here, an IRQ
 * landing on the same CPU mid-critical-section will spin forever waiting for
 * a lock the preempted thread holds (classic same-CPU spinlock deadlock). */
static inline uint64_t mouse_lock_irqsave(void) {
    uint64_t flags = irq_save();
    spinlock_lock(&mouse_lock);
    return flags;
}

static inline void mouse_unlock_irqrestore(uint64_t flags) {
    spinlock_unlock(&mouse_lock);
    irq_restore(flags);
}

/* Polled fallback: drain any PS/2 mouse bytes sitting in the controller's
 * output buffer. Called from the kernel main loop. If IRQ12 isn't being
 * delivered for any reason (bad IOAPIC routing, ISA bridge quirks, etc.), the
 * mouse will still work — we just sample the controller every iteration.
 * Each byte is fed through the exact same packet-state machine the IRQ
 * handler uses, so behaviour is identical.
 *
 * NOTE: bit 5 of status (0x64) means "AUX output buffer full" (data is from
 * mouse, not keyboard). We only consume bytes when both bit 0 (output full)
 * AND bit 5 are set, so we never steal a keyboard byte. */
void mouse_poll(void) {
    static volatile uint64_t poll_calls = 0;
    static volatile bool first_call_logged = false;
    static volatile bool first_byte_logged = false;
    static volatile uint8_t last_status_seen = 0xFF;
    static volatile uint32_t polled_bytes = 0;

    poll_calls++;

    /* Confirm the function is being entered at all. */
    if (!first_call_logged) {
        first_call_logged = true;
        serial_printf("[mouse] POLL: first invocation, mouse_initialized=%d\n",
                      mouse_initialized ? 1 : 0);
    }

    if (!mouse_initialized) return;

    uint64_t flags = mouse_lock_irqsave();

    uint8_t status0 = inb(0x64);
    if (status0 != last_status_seen) {
        last_status_seen = status0;
    }

    int drained = 0;
    /* Bound the work per call to avoid spending too long in one iteration. */
    while (drained < 16) {
        uint8_t status = inb(0x64);
        /* Only consume AUX bytes (OBF and status bit 5). Keyboard scancodes
         * must stay in the buffer for IRQ1 / keyboard_poll. Treating every
         * OBF byte as a mouse packet swallowed Hyper-V keystrokes: Gen 1
         * often posts the byte before IRQ1 is delivered, and QEMU usually
         * delivers IRQ1 first so the race never showed up there. */
        if ((status & 0x01) == 0) break;
        if ((status & 0x20) == 0) break;
        uint8_t data = inb(0x60);
        polled_bytes++;
        drained++;

        if (mouse_packet_pos == 0 && (data & 0x08) == 0) {
            continue;
        }
        mouse_packet[mouse_packet_pos++] = data;
        if (mouse_packet_pos >= mouse_packet_size) {
            mouse_process_packet();
            mouse_packet_pos = 0;
        }
    }
    mouse_unlock_irqrestore(flags);

    if (drained > 0 && !first_byte_logged) {
        first_byte_logged = true;
        serial_printf("[mouse] POLL: first byte drained=%d (after %llu poll calls)\n",
                      drained, (unsigned long long)poll_calls);
    }
#ifdef MOUSE_DEBUG_PACKETS
    /* Periodic heartbeat every 1024 polled bytes (~256 packets). */
    if (drained > 0 && (polled_bytes & 0x3FF) == 0) {
        serial_printf("[mouse] POLL heartbeat: bytes=%u calls=%llu\n",
                      polled_bytes, (unsigned long long)poll_calls);
    }
#endif

    /* Heartbeat: log much less frequently to avoid drowning serial output. */
    if ((poll_calls % 500000) == 0) {
        serial_printf("[mouse] POLL alive: calls=%llu drained_total=%u status=0x%02x\n",
                      (unsigned long long)poll_calls, polled_bytes, status0);
    }
}

// Get current mouse position (for debugging/status)
void mouse_get_position(int *x, int *y) {
    uint64_t flags = mouse_lock_irqsave();
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    mouse_unlock_irqrestore(flags);
}

// Get current button states
uint8_t mouse_get_buttons(void) {
    uint64_t flags = mouse_lock_irqsave();
    uint8_t b = mouse_buttons;
    mouse_unlock_irqrestore(flags);
    return b;
}



// Check if mouse is initialized
bool mouse_is_initialized(void) {
    return mouse_initialized;
}

// Scroll wheel support
int8_t mouse_get_scroll_delta(void) {
    uint64_t flags = mouse_lock_irqsave();
    int8_t delta = scroll_delta_accum;
    scroll_delta_accum = 0;
    mouse_unlock_irqrestore(flags);
    return delta;
}

bool mouse_has_scroll_wheel(void) {
    return intellimouse_enabled;
}

void mouse_sync_screen_bounds(void)
{
    extern uint32_t fb_get_width(void);
    extern uint32_t fb_get_height(void);
    screen_width = (int)fb_get_width();
    screen_height = (int)fb_get_height();
    if (screen_width <= 0)
        screen_width = 1024;
    if (screen_height <= 0)
        screen_height = 768;
    if (mouse_x >= screen_width)
        mouse_x = screen_width - 1;
    if (mouse_y >= screen_height)
        mouse_y = screen_height - 1;
    if (mouse_x < 0)
        mouse_x = 0;
    if (mouse_y < 0)
        mouse_y = 0;
}

void mouse_feed_usb_relative(int8_t dx, int8_t dy, uint8_t buttons)
{
    uint64_t flags = mouse_lock_irqsave();

    int new_x = mouse_x + (int)dx;
    int new_y = mouse_y + (int)dy;

    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x >= screen_width) new_x = screen_width - 1;
    if (new_y >= screen_height) new_y = screen_height - 1;

    if (dx != 0 || dy != 0) {
        sdl_mouse_accum_dx += (int)dx;
        sdl_mouse_accum_dy += (int)dy;
        mouse_x = new_x;
        mouse_y = new_y;
#ifdef ENABLE_LUA
        luade_mouse_event(mouse_x, mouse_y, 0, false);
#endif
    }

    for (int button = 1; button <= 3; button++) {
        uint8_t mask = 1u << (button - 1);
        bool was_pressed = (mouse_buttons & mask) != 0;
        bool is_pressed = (buttons & mask) != 0;
        if (was_pressed != is_pressed) {
#ifdef ENABLE_LUA
            luade_mouse_event(mouse_x, mouse_y, button, is_pressed);
#endif
        }
    }

    mouse_buttons = buttons;
    mouse_unlock_irqrestore(flags);
}
