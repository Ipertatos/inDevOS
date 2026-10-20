#include "keyboard.h"
extern bool desktop_is_running(void);
#include "apic.h"
#include "acpi.h"
#include "cpu.h"
#include "utils.h"
#include "mem.h"
#include "atomic.h"
#include "debug.h"
#include "serial.h"
#include "term.h"
#include "input.h"
#include "hpet.h"
#include "../include/SDL.h"
#include "vconsole.h"

/* Optional SDL: PS/2 path calls SDL_InjectKeyEvent (sdl_events.c); no-op if events not ready */
extern void SDL_InjectKeyEvent(bool is_down, uint8_t scancode);
/* IRQ-safe state-only update: must be called from the keyboard ISR so that
 * user apps polling SDL_GetKeyboardState() (e.g. DOOM) see key transitions
 * even when the kernel main-loop drainer is starved by a synchronous Ring-3
 * exec. Implementation in sdl_events.c. */
extern void SDL_InjectKeyState_Fast(bool is_down, uint8_t scancode);

static bool shift_pressed = false;
static bool left_ctrl_pressed = false;
static bool left_alt_pressed = false;
static bool super_pressed = false;
static bool e0_prefix = false;
static bool capsLock = false;

static uint16_t keyboard_sdl_mod_mask(void) {
    uint16_t m = KMOD_NONE;
    if (shift_pressed) {
        m |= KMOD_LSHIFT | KMOD_RSHIFT;
    }
    if (left_ctrl_pressed) {
        m |= KMOD_LCTRL;
    }
    return m;
}

/* kbd_buf, kbd_len, kbd_execute, raw_mode, terminate_flag, force_kill_flag,
 * program_running, and last_ctrlc_tick now live per-VC inside vconsole_t.
 * The ISR operates on the active VC; consumers read from the process's VC. */

static volatile bool tab_requested = false;

static volatile bool pgup_pressed = false;
static volatile bool pgdn_pressed = false;

static volatile bool arrow_up_pressed = false;
static volatile bool arrow_down_pressed = false;
static volatile bool arrow_left_pressed = false;
static volatile bool arrow_right_pressed = false;
static volatile bool escape_pressed = false;
static volatile bool home_pressed = false;
static volatile bool end_pressed = false;
static volatile bool kill_line_pressed = false;
static volatile bool kill_word_pressed = false;
/* Set when a program put a VC in raw mode; its leftover bytes are raw keys,
 * not shell type-ahead, so keyboard_restore_shell_input() drops them. */
static volatile bool kbd_raw_used = false;
static volatile bool f1_pressed = false;
static volatile bool f5_pressed = false;

static volatile bool vconsole_switch_requested = false;

#define CTRLC_DOUBLE_PRESS_WINDOW_MS 2000

// Spinlock to protect keyboard state and buffer with canaries for corruption detection
static uint64_t canary_before = 0xDEADBEEFCAFEBABE;
static spinlock_t keyboard_lock = {0};

/* i8042 data port ownership.  IRQ1, IRQ12, keyboard_poll() and mouse_poll()
 * all read port 0x60, possibly on different CPUs.  Reading 0x60 when the
 * output buffer is already empty returns the previous byte again (QEMU and
 * real 8042s), so a poll on one CPU racing the IRQ1 handler on another
 * delivered the same make code twice ("eecho").  Every reader now takes this
 * lock, checks OBF (status bit 0) and the AUX bit (5) and only then reads. */
spinlock_t i8042_lock = {0};
static volatile uint32_t kbd_irq_empty;       /* IRQ1 found OBF clear: byte already taken */
static uint64_t canary_after = 0xFEEDFACEDEADC0DE;

// Safe interrupt-context echo queue
#define ECHO_QUEUE_SIZE 256
static char echo_queue[ECHO_QUEUE_SIZE];
static volatile uint32_t echo_head = 0;
static volatile uint32_t echo_tail = 0;
static volatile bool echo_pending = false;

// Deferred SDL key event queue (ISR enqueues, main loop drains)
typedef struct { uint8_t scancode; bool is_down; } sdl_key_evt_t;
#define SDL_KEY_QUEUE_SIZE 64
static sdl_key_evt_t sdl_key_queue[SDL_KEY_QUEUE_SIZE];
static volatile uint32_t sdl_key_head = 0;
static volatile uint32_t sdl_key_tail = 0;
static volatile bool sdl_key_pending = false;

/* ================================================================
 * Keyboard layout support
 * ================================================================ */
static int current_layout = KB_LAYOUT_US; // Default US layout

const uint32_t UNKNOWN = 0xFFFFFFFF,
               ESC = 0xFFFFFFFF - 1,
               CTRL = 0xFFFFFFFF - 2,
               LSHFT = 0xFFFFFFFF - 3,
               RSHFT = 0xFFFFFFFF - 4,
               ALT = 0xFFFFFFFF - 5,
               F1 = 0xFFFFFFFF - 6,
               F2 = 0xFFFFFFFF - 7,
               F3 = 0xFFFFFFFF - 8,
               F4 = 0xFFFFFFFF - 9,
               F5 = 0xFFFFFFFF - 10,
               F6 = 0xFFFFFFFF - 11,
               F7 = 0xFFFFFFFF - 12,
               F8 = 0xFFFFFFFF - 13,
               F9 = 0xFFFFFFFF - 14,
               F10 = 0xFFFFFFFF - 15,
               F11 = 0xFFFFFFFF - 16,
               F12 = 0xFFFFFFFF - 17,
               SCRLCK = 0xFFFFFFFF - 18,
               HOME = 0xFFFFFFFF - 19,
               UP = 0xFFFFFFFF - 20,
               LEFT = 0xFFFFFFFF - 21,
               RIGHT = 0xFFFFFFFF - 22,
               DOWN = 0xFFFFFFFF - 23,
               PGUP = 0xFFFFFFFF - 24,
               PGDOWN = 0xFFFFFFFF - 25,
               END = 0xFFFFFFFF - 26,
               INS = 0xFFFFFFFF - 27,
               DEL = 0xFFFFFFFF - 28,
               CAPS = 0xFFFFFFFF - 29,
               NONE = 0xFFFFFFFF - 30,
               ALTGR = 0xFFFFFFFF - 31,
               NUMLCK = 0xFFFFFFFF - 32;

const uint32_t lowercase[128] = {
UNKNOWN,ESC,'1','2','3','4','5','6','7','8',
'9','0','-','=','\b','\t','q','w','e','r',
't','y','u','i','o','p','[',']','\n',CTRL,
'a','s','d','f','g','h','j','k','l',';',
'\'','`',LSHFT,'\\','z','x','c','v','b','n','m',',',
'.','/',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',LEFT,UNKNOWN,RIGHT,
'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,UNKNOWN,F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

const uint32_t uppercase[128] = {
    UNKNOWN,ESC,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R',
'T','Y','U','I','O','P','{','}','\n',CTRL,'A','S','D','F','G','H','J','K','L',':','"','~',LSHFT,'|','Z','X','C',
'V','B','N','M','<','>','?',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',
LEFT,UNKNOWN,RIGHT,'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,UNKNOWN,F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

/* --- UK layout: #, £, etc. --- */
static const uint32_t uk_lowercase[128] = {
UNKNOWN,ESC,'1','2','3','4','5','6','7','8',
'9','0','-','=','\b','\t','q','w','e','r',
't','y','u','i','o','p','[',']','\n',CTRL,
'a','s','d','f','g','h','j','k','l',';',
'\'','`',LSHFT,'#','z','x','c','v','b','n','m',',',
'.','/',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',LEFT,UNKNOWN,RIGHT,
'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,'\\',F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

static const uint32_t uk_uppercase[128] = {
UNKNOWN,ESC,'!','"','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R',
'T','Y','U','I','O','P','{','}','\n',CTRL,'A','S','D','F','G','H','J','K','L',':','@','~',LSHFT,'~','Z','X','C',
'V','B','N','M','<','>','?',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',
LEFT,UNKNOWN,RIGHT,'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,'|',F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

/* --- German QWERTZ layout --- */
static const uint32_t de_lowercase[128] = {
UNKNOWN,ESC,'1','2','3','4','5','6','7','8',
'9','0','-','=','\b','\t','q','w','e','r',
't','z','u','i','o','p','[',']','\n',CTRL,
'a','s','d','f','g','h','j','k','l',';',
'\'','`',LSHFT,'#','y','x','c','v','b','n','m',',',
'.','/',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',LEFT,UNKNOWN,RIGHT,
'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,'<',F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

static const uint32_t de_uppercase[128] = {
UNKNOWN,ESC,'!','"','#','$','%','&','/','(',')','=','?','`','\b','\t','Q','W','E','R',
'T','Z','U','I','O','P','{','}','\n',CTRL,'A','S','D','F','G','H','J','K','L',':','\'','~',LSHFT,'\'','Y','X','C',
'V','B','N','M',';',':','_',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',
LEFT,UNKNOWN,RIGHT,'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,'>',F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

/* --- French AZERTY layout --- */
static const uint32_t fr_lowercase[128] = {
UNKNOWN,ESC,'&','e','"','\'','(','_','e','!',
'c','a',')','-','\b','\t','a','z','e','r',
't','y','u','i','o','p','^','$','\n',CTRL,
'q','s','d','f','g','h','j','k','l','m',
'u','*',LSHFT,'<','w','x','c','v','b','n',',',';',
':','!',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',LEFT,UNKNOWN,RIGHT,
'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,'<',F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

static const uint32_t fr_uppercase[128] = {
UNKNOWN,ESC,'1','2','3','4','5','6','7','8',
'9','0','+','=','\b','\t','A','Z','E','R',
'T','Y','U','I','O','P','^','$','\n',CTRL,
'Q','S','D','F','G','H','J','K','L','M',
'%','~',LSHFT,'>','W','X','C','V','B','N','?','.',
'/',UNKNOWN,RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',
LEFT,UNKNOWN,RIGHT,'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,'>',F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

/* Layout table – indexed by KB_LAYOUT_* constants */
static const uint32_t* layout_lower[KB_LAYOUT_MAX] = {
    lowercase,     /* US */
    uk_lowercase,  /* UK */
    de_lowercase,  /* DE */
    fr_lowercase,  /* FR */
};
static const uint32_t* layout_upper[KB_LAYOUT_MAX] = {
    uppercase,     /* US */
    uk_uppercase,  /* UK */
    de_uppercase,  /* DE */
    fr_uppercase,  /* FR */
};
static const char* layout_names[KB_LAYOUT_MAX] = {
    "US (QWERTY)", "UK (QWERTY)", "DE (QWERTZ)", "FR (AZERTY)"
};

// Check if there are characters to echo
bool keyboard_has_echo_pending(void) {
    return echo_pending;
}

// Safe interrupt-context echo - just queue characters for main thread
static void safe_echo_char(char c) {
    uint32_t next_head = (echo_head + 1) % ECHO_QUEUE_SIZE;
    if (next_head != echo_tail) {
        echo_queue[echo_head] = c;
        echo_head = next_head;
        echo_pending = true;
    }
}

/* Wait for the PS/2 controller's input buffer to drain (status bit 1 == 0)
 * before issuing the next command. The original code inverted these loops
 * (`while ((inb(0x64) & 2) != 0) { outb(...); }`), which never sent the
 * command on a normal boot because the buffer is already empty — leaving the
 * keyboard controller in whatever state the BIOS handed us. */
static void kbd_wait_input_empty(void) {
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout > 0) {
        /* spin */
    }
}

static bool kbd_wait_output_full(void) {
    int timeout = 100000;
    while (!(inb(0x64) & 0x01) && --timeout > 0) {
        /* spin */
    }
    return timeout > 0;
}

static void kbd_flush_output(void) {
    for (int i = 0; i < 16; i++) {
        if ((inb(0x64) & 0x01) == 0) break;
        inb(0x60);
    }
}

/* Byte written to 0x60 with no command pending goes to the keyboard device. */
static bool kbd_dev_command(uint8_t cmd) {
    kbd_wait_input_empty();
    outb(0x60, cmd);
    if (!kbd_wait_output_full()) return false;
    return inb(0x60) == 0xFA;
}

void keyboard_init(){
    printf("PS/2 Keyboard: Starting initialization\n");

    /* Hyper-V Gen 1's i8042 is a real PS/2 keyboard, but it is pickier than
     * QEMU. Command 0xA7 (disable aux) can wedge controllers that have no
     * second port, and a timed-out config read returns 0xFF on the open
     * data bus. Writing 0xFF back sets bit 4 and disables the keyboard
     * clock, so IRQ1 never carries keystrokes. QEMU always answers 0x20,
     * which is why typing worked there. Do not touch the aux port here
     * (mouse_init owns it). Never write a config byte we did not actually
     * read. */

    kbd_wait_input_empty();
    outb(0x64, 0xAD); /* disable first PS/2 port while reconfiguring */
    kbd_flush_output();

    kbd_wait_input_empty();
    outb(0x64, 0x20);
    uint8_t config = 0x45; /* IRQ1 + system flag + scancode translation */
    if (kbd_wait_output_full()) {
        uint8_t got = inb(0x60);
        if (got != 0xFF) {
            config = got;
        } else {
            printf("PS/2 Keyboard: config byte was 0xFF, using safe default\n");
        }
    } else {
        printf("PS/2 Keyboard: config read timed out, using safe default\n");
    }

    /* bit0 IRQ1, bit6 translation. bit4 clear = keyboard clock enabled. */
    config |= (1 << 0) | (1 << 6);
    config &= (uint8_t)~(1 << 4);

    kbd_wait_input_empty();
    outb(0x64, 0x60);
    kbd_wait_input_empty();
    outb(0x60, config);

    kbd_wait_input_empty();
    outb(0x64, 0xAE); /* enable keyboard port */

    /* Port enable is not device scanning. Hyper-V stops reporting keys
     * after the interface is disabled until the device gets 0xF4. */
    kbd_flush_output();
    if (!kbd_dev_command(0xF4)) {
        printf("PS/2 Keyboard: enable-scanning (0xF4) was not ACKed\n");
    }

    if (!input_ps2_kbd) {
        input_dev_t *d = input_allocate("AT Translated Set 2 keyboard", BUS_I8042, 0x0001, 0x0001);
        if (d) {
            input_setup_keyboard(d);
            if (input_register(d) == 0) input_ps2_kbd = d;
        }
    }

    printf("PS/2 Keyboard: config 0x%02x, IOAPIC redirect (LAPIC ID: %d, Vector: 33, IRQ: 1)\n",
           config, bsp_lapic_id);
    ioapic_redirect_irq(bsp_lapic_id, 33, 1, true);

    printf("PS/2 Keyboard: Initialization complete\n");
    printf("PS/2 Keyboard: Controller status: 0x%02x\n", inb(0x64));
}
char ps2_translate2ascii(uint8_t code, bool shift, bool caps) {
    if(code >= 128) return 0;
    /* Use layout-specific tables */
    int layout = current_layout;
    if (layout < 0 || layout >= KB_LAYOUT_MAX) layout = KB_LAYOUT_US;
    const uint32_t *lower = layout_lower[layout];
    const uint32_t *upper = layout_upper[layout];
    uint32_t val;
    if (shift ^ caps) {
        val = upper[code];
    } else {
        val = lower[code];
    }
    if (val == UNKNOWN || val > 127) return 0;
    return (char)val;
}

bool keyboard_get_alt(void) { return left_alt_pressed; }
bool keyboard_get_super(void) { return super_pressed; }

static void keyboard_process_byte(uint16_t scancode){
    spinlock_lock(&keyboard_lock);
    if (scancode == 0xE0) {
        e0_prefix = true;
        spinlock_unlock(&keyboard_lock);
        return;
    }
    bool ext = e0_prefix;
    e0_prefix = false;
    bool is_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    /* Left GUI / Super: E0 5B, or bare 0x5B from some QEMU keymaps. */
    if (code == 0x5B) {
        super_pressed = !is_release;
        SDL_InjectKeyState_Fast(!is_release, 0x5B);
        uint32_t next = (sdl_key_head + 1) % SDL_KEY_QUEUE_SIZE;
        if (next != sdl_key_tail) {
            sdl_key_queue[sdl_key_head].scancode = 0x5B;
            sdl_key_queue[sdl_key_head].is_down  = !is_release;
            sdl_key_head = next;
            sdl_key_pending = true;
        }
        spinlock_unlock(&keyboard_lock);
        return;
    }

    /* Kernel chords - Ctrl+F1..F4 (VT switch) and Ctrl+Q (VT cycle) - belong
     * to the kernel: neither the press nor its matching release (whenever it
     * comes) is queued to apps (SDL state/queue -> desktop windows, /dev/wm,
     * SDL programs).  Plain F-keys and other Ctrl chords still reach apps.
     * Evdev readers keep seeing the raw keys, as on Linux. */
    static bool kchord_held[128];
    bool kchord = false;
    if (!ext) {
        if (!is_release && left_ctrl_pressed &&
            ((code >= 0x3B && code <= 0x3E) || code == 0x10 /*q*/)) {
            kchord_held[code] = true;
            kchord = true;
        } else if (is_release && kchord_held[code]) {
            kchord_held[code] = false;
            kchord = true;
        }
    }

    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());

    /* Update SDL keyboard state immediately so polling user apps (DOOM,
     * etc.) see key transitions even when the main-loop drainer is
     * starved by a long-running Ring-3 process. The byte store is atomic
     * and lock-free, so it's safe from interrupt context. */
    /* With the desktop running, presses typed on a text VT belong to that
     * VT only; releases always pass so nothing sticks in the desktop. */
    bool to_gfx = !kchord &&
                  (is_release || !vconsole_gfx_owner_running() || vconsole_get_active() == 0);
    if (to_gfx)
        SDL_InjectKeyState_Fast(!is_release, code);

    /* Defer the full SDL event push (event_queue spinlock + desktop GUI
     * notify) to main-loop context; ISR must stay minimal. */
    if (to_gfx) {
        uint32_t next = (sdl_key_head + 1) % SDL_KEY_QUEUE_SIZE;
        if (next != sdl_key_tail) {
            sdl_key_queue[sdl_key_head].scancode = code;
            sdl_key_queue[sdl_key_head].is_down  = !is_release;
            sdl_key_head = next;
            sdl_key_pending = true;
        }
    }

    /* Same for the native desktop: while it owns VT 0, presses reach it
     * through the SDL queue above only. Feeding them to the hidden Zeus
     * line on VT 0 as well made Zeus run half-typed commands (and block
     * the main loop in a sync program) while typing into a window. */
    if (!is_release && desktop_is_running() && vconsole_get_active() == 0) {
        bool vt_chord = left_ctrl_pressed &&
                        ((code >= 0x3B && code <= 0x3E) || code == 0x10 /*q*/);
        bool modifier = code == 0x1D || code == 0x2A || code == 0x36 ||
                        code == 0x38 || code == 0x3A;
        if (!vt_chord && !modifier) goto end;
    }
    if (is_release) {
        switch(code) {
            case 0x1D: left_ctrl_pressed = false; break;
            case 0x2A:
            case 0x36: shift_pressed = false; break;
            case 0x38: left_alt_pressed = false; break;
        }
        spinlock_unlock(&keyboard_lock);
        return;
    }
    switch(code) {
        case 0x1D: left_ctrl_pressed = true; goto end;
        case 0x2A:
        case 0x36: shift_pressed = true; goto end;
        case 0x38: left_alt_pressed = true; goto end;
        case 0x3A: capsLock = !capsLock; goto end;
    }
    /* Raw-mode (termios ~ICANON) Linux programs such as busybox vi/less
     * read keys from kbd_buf: deliver Esc, arrows and the nav block as the
     * VT100 sequences they expect instead of the Zeus-only key flags. */
    if (vc && (vc->raw_mode || vc->lx_noncanon)) {
        const char *seq = NULL;
        switch (code) {
            case 0x01: seq = "\x1b"; break;
            case 0x48: seq = "\x1b[A"; break;
            case 0x50: seq = "\x1b[B"; break;
            case 0x4D: seq = "\x1b[C"; break;
            case 0x4B: seq = "\x1b[D"; break;
            case 0x47: seq = "\x1b[H"; break;
            case 0x4F: seq = "\x1b[F"; break;
            case 0x49: seq = "\x1b[5~"; break;
            case 0x51: seq = "\x1b[6~"; break;
            case 0x53: seq = "\x1b[3~"; break;
        }
        if (seq) {
            for (; *seq && vc->kbd_len < sizeof(vc->kbd_buf) - 1; seq++)
                vc->kbd_buf[vc->kbd_len++] = *seq;
            vc->kbd_buf[vc->kbd_len] = '\0';
            vc->kbd_cursor = vc->kbd_len;
            goto end;
        }
    }
    if (code == 0x01) { escape_pressed = true; goto end; }
    if (code == 0x48) { arrow_up_pressed = true; goto end; }
    if (code == 0x50) { arrow_down_pressed = true; goto end; }
    if (code == 0x4B) { arrow_left_pressed = true; goto end; }
    if (code == 0x4D) { arrow_right_pressed = true; goto end; }
    if (code == 0x49) { pgup_pressed = true; goto end; }
    if (code == 0x51) { pgdn_pressed = true; goto end; }
    if (code == 0x47) { home_pressed = true; goto end; }
    if (code == 0x4F) { end_pressed = true; goto end; }
    /* Ctrl+F1..F4 → tty0..tty3 (F1 without Ctrl keeps the existing F1 hook). */
    if (code >= 0x3B && code <= 0x3E) {
        if (left_ctrl_pressed) {
            vconsole_request_switch(code - 0x3B);
            goto end;
        }
        if (code == 0x3B) f1_pressed = true;
        goto end;
    }
    if (code == 0x3F) { f5_pressed = true; goto end; }

    char ch = ps2_translate2ascii(code, shift_pressed, capsLock);
    if (ch == 0) {
        goto end;
    }

    /* Line-editing chords (not consumed in raw mode). */
    if (left_ctrl_pressed && !vc->raw_mode) {
        if (ch == 'a') { home_pressed = true; goto end; }
        if (ch == 'e') { end_pressed = true; goto end; }
        if (ch == 'u') { kill_line_pressed = true; goto end; }
        if (ch == 'w') { kill_word_pressed = true; goto end; }
    }

    /* Ctrl+C: always processed, even in raw mode.
     * Sets the active VC's terminate flag — only the foreground job on
     * this console is affected. */
    /* Linux job control: a Linux program (ash) owns the console. Send
     * SIGINT/SIGQUIT to the foreground pgrp instead of force-killing the
     * sync program (which took ash down). Zeus-native / Doom unchanged. */
    if (left_ctrl_pressed && (ch == 'c' || ch == '\\')) {
        extern bool tty_linux_foreground(void);
        extern void tty_post_signal(int sig);
        if (tty_linux_foreground()) {
            tty_post_signal(ch == 'c' ? 2 /*SIGINT*/ : 3 /*SIGQUIT*/);
            memset(vc->kbd_buf, '\0', sizeof(vc->kbd_buf));
            vc->kbd_len     = 0;
            vc->kbd_execute = false;
            safe_echo_char('^');
            safe_echo_char(ch == 'c' ? 'C' : '\\');
            safe_echo_char('\n');
            goto end;
        }
    }

    if (left_ctrl_pressed && ch == 'c') {
        uint64_t now = hpet_get_ticks();
        if (vc->terminate_flag) {
            uint64_t elapsed_ms = now - vc->last_ctrlc_tick;
            if (elapsed_ms < CTRLC_DOUBLE_PRESS_WINDOW_MS) {
                vc->force_kill_flag = true;
            }
        }
        vc->terminate_flag  = true;
        vc->last_ctrlc_tick = now;
        memset(vc->kbd_buf, '\0', sizeof(vc->kbd_buf));
        vc->kbd_len     = 0;
        vc->kbd_execute = false;
        safe_echo_char('^');
        safe_echo_char('C');
        safe_echo_char('\n');
        goto end;
    }

    /* Ctrl+Q: cycle TTYs (also works in raw mode) */
    if (left_ctrl_pressed && ch == 'q') {
        vconsole_request_switch(-2);
        goto end;
    }

    /* ---- Raw mode: buffer only, no echo, no execute flag ---- */
    if (vc->raw_mode) {
        if (vc->kbd_len < sizeof(vc->kbd_buf) - 1) {
            vc->kbd_buf[vc->kbd_len] = ch;
            vc->kbd_len++;
        }
        goto end;
    }

    if (ch == '\t') {
        tab_requested = true;
        goto end;
    }

    if (ch == '\n') {
        vc->kbd_cursor = vc->kbd_len;
        if (vc->kbd_len < sizeof(vc->kbd_buf) - 1)
            vc->kbd_buf[vc->kbd_len++] = ch;
        vc->kbd_buf[vc->kbd_len] = '\0';
        safe_echo_char(ch);
        vc->kbd_execute = true;
        goto end;
    }

    if (ch == '\b') {
        if (vc->kbd_cursor > 0 && vc->kbd_len > 0) {
            uint32_t cur = vc->kbd_cursor - 1;
            memmove(vc->kbd_buf + cur, vc->kbd_buf + cur + 1,
                    vc->kbd_len - cur - 1);
            vc->kbd_len--;
            vc->kbd_cursor = cur;
            vc->kbd_buf[vc->kbd_len] = '\0';
            if (vc->kbd_cursor == vc->kbd_len) {
                serial_puts(" \b");
                safe_echo_char('\b');
                safe_echo_char(' ');
                safe_echo_char('\b');
            } else {
                vc->kbd_dirty = true;
            }
        }
        goto end;
    }

    if (vc->kbd_len < sizeof(vc->kbd_buf) - 1) {
        uint32_t cur = vc->kbd_cursor;
        if (cur > vc->kbd_len) cur = vc->kbd_len;
        if (cur < vc->kbd_len)
            memmove(vc->kbd_buf + cur + 1, vc->kbd_buf + cur, vc->kbd_len - cur);
        vc->kbd_buf[cur] = ch;
        vc->kbd_len++;
        vc->kbd_cursor = cur + 1;
        vc->kbd_buf[vc->kbd_len] = '\0';
        if (vc->kbd_cursor == vc->kbd_len)
            safe_echo_char(ch);
        else
            vc->kbd_dirty = true;
    }

end:
    spinlock_unlock(&keyboard_lock);
}

/* USB HID keyboards translate their reports to set-1 bytes and feed them
 * here, so they share all PS/2 key handling. */
void keyboard_inject_scancode(uint8_t b) {
    uint64_t flags = irq_save();
    keyboard_process_byte(b);
    irq_restore(flags);
}

void keyboard_handler(void) {
    spinlock_lock(&i8042_lock);                 /* IRQs are off in the handler */
    uint8_t st = inb(0x64);
    if ((st & 0x01) == 0 || (st & 0x20)) {
        /* Empty (a poller already took the byte) or a mouse byte (IRQ12 /
         * mouse_poll owns it).  Reading 0x60 now would repeat a stale key. */
        spinlock_unlock(&i8042_lock);
        if ((st & 0x01) == 0) {
            uint32_t n = ++kbd_irq_empty;
            if (n <= 5 || (n & 0xFF) == 0)
                serial_printf("kbd: IRQ1 with empty i8042 buffer #%u (byte already consumed; not re-read)\n", n);
        }
        apic_eoi();
        return;
    }
    uint8_t b = inb(0x60);
    spinlock_unlock(&i8042_lock);
    keyboard_process_byte(b);
    input_kbd_set1_byte(input_ps2_kbd, b);     /* /dev/input/eventN */
    apic_eoi();
}

/* Main-loop fallback for when IRQ1 is not delivered.
 * Hyper-V Gen 1 still latches the scancode in the i8042 output buffer.
 * Status bit 5 set means the byte is from the AUX (mouse) port; leave it. */
void keyboard_poll(void) {
    uint64_t flags = irq_save();
    for (int n = 0; n < 16; n++) {
        spinlock_lock(&i8042_lock);
        uint8_t st = inb(0x64);
        if ((st & 0x01) == 0 || (st & 0x20)) { spinlock_unlock(&i8042_lock); break; }
        uint8_t b = inb(0x60);
        spinlock_unlock(&i8042_lock);
        keyboard_process_byte(b);
        input_kbd_set1_byte(input_ps2_kbd, b);
    }
    irq_restore(flags);
}

/* keyboard_get_execute / keyboard_get_buffer / keyboard_get_buffer_length
 * operate on the **active** VC (used by the Zeus shell main loop).
 * keyboard_consume_char operates on the **calling process's** VC so that
 * sys_read(0) / sys_getchar pulls from the correct session. */

bool keyboard_get_execute(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    bool result = vc->kbd_execute;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
    return result;
}

char* keyboard_get_buffer(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    return vc->kbd_buf;
}

uint32_t keyboard_get_buffer_length(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    uint32_t result = vc->kbd_len;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
    return result;
}

void keyboard_clear_execute(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    vc->kbd_execute = false;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
}

void keyboard_clear_buffer(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    memset(vc->kbd_buf, '\0', sizeof(vc->kbd_buf));
    vc->kbd_len     = 0;
    vc->kbd_cursor  = 0;
    vc->kbd_dirty   = false;
    vc->kbd_execute = false;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
}

/* Take the first complete line (through '\n') out of the active VC buffer.
 * Type-ahead after it stays buffered (and stays executable if it holds
 * another '\n'), so keys typed while a program ran are not lost. */
uint32_t keyboard_take_line(char *out, uint32_t outsz) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    uint32_t n = 0;
    while (n < vc->kbd_len && vc->kbd_buf[n] != '\n') n++;
    if (n >= vc->kbd_len) {
        /* Stale execute flag: a stdin reader (e.g. an exiting ash) took
         * the '\n' via keyboard_consume_char(). Keep the partial line. */
        vc->kbd_execute = false;
        spinlock_unlock(&keyboard_lock);
        irq_restore(irq_flags);
        return 0;
    }
    uint32_t take = n + 1;
    uint32_t c = take < outsz - 1 ? take : outsz - 1;
    memcpy(out, vc->kbd_buf, c);
    out[c] = '\0';
    memmove(vc->kbd_buf, vc->kbd_buf + take, vc->kbd_len - take);
    vc->kbd_len -= take;
    vc->kbd_buf[vc->kbd_len] = '\0';
    vc->kbd_cursor = vc->kbd_len;
    vc->kbd_dirty = false;
    bool more = false;
    for (uint32_t i = 0; i < vc->kbd_len; i++)
        if (vc->kbd_buf[i] == '\n') { more = true; break; }
    vc->kbd_execute = more;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
    return c;
}

int keyboard_consume_char(void) {
    int vc_id = vconsole_get_thread_console();
    vconsole_t *vc = vconsole_get_by_id(vc_id);
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);

    if (vc->kbd_len == 0) {
        spinlock_unlock(&keyboard_lock);
        irq_restore(irq_flags);
        return -1;
    }

    char ch = vc->kbd_buf[0];
    if (ch == '\n')
        vc->kbd_execute = false;   /* reader took the line */
    if (vc->kbd_len > 1) {
        memmove(vc->kbd_buf, vc->kbd_buf + 1, vc->kbd_len - 1);
    }
    vc->kbd_len--;
    vc->kbd_buf[vc->kbd_len] = '\0';
    if (vc->kbd_cursor > 0)
        vc->kbd_cursor--;

    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
    return (int)(unsigned char)ch;
}

// Process queued echo characters (called from main thread)
void keyboard_process_echo(void) {
    while (echo_tail != echo_head) {
        char c = echo_queue[echo_tail];
        echo_tail = (echo_tail + 1) % ECHO_QUEUE_SIZE;
        
        // Safe printf call from main thread context
        printf("%c", c);
    }
    echo_pending = false;
}

/* ================================================================
 * Deferred SDL key event processing (called from main thread)
 * ================================================================ */
bool keyboard_has_sdl_pending(void) {
    return sdl_key_pending;
}

void keyboard_process_sdl_events(void) {
    while (sdl_key_tail != sdl_key_head) {
        sdl_key_evt_t evt = sdl_key_queue[sdl_key_tail];
        sdl_key_tail = (sdl_key_tail + 1) % SDL_KEY_QUEUE_SIZE;
        SDL_InjectKeyEvent(evt.is_down, evt.scancode);
    }
    sdl_key_pending = false;
}

/* ================================================================
 * Tab completion accessors (called from main thread)
 * ================================================================ */
bool keyboard_get_tab_requested(void) {
    return tab_requested;
}

void keyboard_clear_tab_requested(void) {
    tab_requested = false;
}

/* ================================================================
 * Scrollback accessors (called from main thread)
 * ================================================================ */
bool keyboard_get_pgup_pressed(void) {
    return pgup_pressed;
}

bool keyboard_get_pgdn_pressed(void) {
    return pgdn_pressed;
}

void keyboard_clear_pgup(void) {
    pgup_pressed = false;
}

void keyboard_clear_pgdn(void) {
    pgdn_pressed = false;
}

bool keyboard_get_arrow_up(void) {
    return arrow_up_pressed;
}

bool keyboard_get_arrow_down(void) {
    return arrow_down_pressed;
}

void keyboard_clear_arrow_up(void) {
    arrow_up_pressed = false;
}

void keyboard_clear_arrow_down(void) {
    arrow_down_pressed = false;
}

bool keyboard_get_arrow_left(void) {
    return arrow_left_pressed;
}

bool keyboard_get_arrow_right(void) {
    return arrow_right_pressed;
}

void keyboard_clear_arrow_left(void) {
    arrow_left_pressed = false;
}

void keyboard_clear_arrow_right(void) {
    arrow_right_pressed = false;
}

bool keyboard_get_escape(void) {
    return escape_pressed;
}

void keyboard_clear_escape(void) {
    escape_pressed = false;
}

bool keyboard_get_home(void) {
    return home_pressed;
}

void keyboard_clear_home(void) {
    home_pressed = false;
}

bool keyboard_get_end(void) {
    return end_pressed;
}

void keyboard_clear_end(void) {
    end_pressed = false;
}

bool keyboard_get_kill_line(void) {
    return kill_line_pressed;
}

void keyboard_clear_kill_line(void) {
    kill_line_pressed = false;
}

bool keyboard_get_kill_word(void) {
    return kill_word_pressed;
}

void keyboard_clear_kill_word(void) {
    kill_word_pressed = false;
}

uint32_t keyboard_get_cursor(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    return vc ? vc->kbd_cursor : 0;
}

void keyboard_set_cursor(uint32_t pos) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    if (!vc) return;
    if (pos > vc->kbd_len) pos = vc->kbd_len;
    vc->kbd_cursor = pos;
}

bool keyboard_take_dirty(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    if (!vc || !vc->kbd_dirty) return false;
    vc->kbd_dirty = false;
    return true;
}

void keyboard_delete_range(uint32_t start, uint32_t end) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    if (!vc) return;
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    if (end > vc->kbd_len) end = vc->kbd_len;
    if (start > end) start = end;
    uint32_t n = end - start;
    if (n) {
        memmove(vc->kbd_buf + start, vc->kbd_buf + end, vc->kbd_len - end);
        vc->kbd_len -= n;
        vc->kbd_buf[vc->kbd_len] = '\0';
    }
    vc->kbd_cursor = start;
    vc->kbd_dirty = true;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
}

bool keyboard_get_f1(void) {
    return f1_pressed;
}

void keyboard_clear_f1(void) {
    f1_pressed = false;
}

bool keyboard_get_f5(void) {
    return f5_pressed;
}

void keyboard_clear_f5(void) {
    f5_pressed = false;
}

bool keyboard_get_vconsole_switch(void) {
    return vconsole_switch_requested;
}

void keyboard_clear_vconsole_switch(void) {
    vconsole_switch_requested = false;
}

void keyboard_set_linux_noncanon(bool enable) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    if (vc) vc->lx_noncanon = enable;
}

void keyboard_set_raw_mode(bool enable) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    if (!enable && vc->raw_mode) {
        /* Leaving raw: discard only what the program buffered in raw mode.
         * Keys typed after this (still before restore) are cooked type-ahead. */
        memset(vc->kbd_buf, 0, sizeof(vc->kbd_buf));
        vc->kbd_len     = 0;
        vc->kbd_cursor  = 0;
        vc->kbd_dirty   = false;
        vc->kbd_execute = false;
        echo_tail    = echo_head;
        echo_pending = false;
        kbd_raw_used = false;
    }
    vc->raw_mode = enable;
    if (!enable) vc->lx_noncanon = false;
    if (enable) {
        kbd_raw_used = true;
        arrow_up_pressed = false;
        arrow_down_pressed = false;
        arrow_left_pressed = false;
        arrow_right_pressed = false;
        escape_pressed = false;
        home_pressed = false;
        end_pressed = false;
        pgup_pressed = false;
        pgdn_pressed = false;
        f1_pressed = false;
        f5_pressed = false;
        memset(vc->kbd_buf, 0, sizeof(vc->kbd_buf));
        vc->kbd_len     = 0;
        vc->kbd_execute = false;
    }
}

bool keyboard_get_raw_mode(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    return vc->raw_mode;
}

void keyboard_set_buffer(const char *str, uint32_t len) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    if (len >= sizeof(vc->kbd_buf)) len = sizeof(vc->kbd_buf) - 1;
    memcpy(vc->kbd_buf, str, len);
    vc->kbd_buf[len] = '\0';
    vc->kbd_len = len;
    vc->kbd_cursor = len;
    vc->kbd_dirty = false;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
}

void keyboard_inject_string(const char *s, int len) {
    if (!s || len <= 0) return;
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    for (int i = 0; i < len && vc->kbd_len < sizeof(vc->kbd_buf) - 1; i++) {
        vc->kbd_buf[vc->kbd_len++] = s[i];
    }
    vc->kbd_buf[vc->kbd_len] = '\0';
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
}

/* Feed a single ASCII character into the active VC's keyboard buffer.
 * Used to unify USB keyboard input with the PS/2 path so both sources
 * share the same per-VC buffer, echo, and line-editing logic. */
void keyboard_restore_shell_input(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);
    vc->terminate_flag   = false;
    vc->force_kill_flag  = false;
    vc->program_running  = false;
    /* Always drop echo queued while the program owned the console. Ash
     * (cooked VC) consumes keys into its own line editor but leaves our
     * echo queue full; replaying it after the Zeus prompt made the screen
     * show ash keystrokes while Zeus's buffer did not match. */
    echo_tail    = echo_head;
    echo_pending = false;
    if (vc->raw_mode || kbd_raw_used) {
        /* Still raw-owned: leftovers are raw scancodes/bytes, not a Zeus line. */
        memset(vc->kbd_buf, 0, sizeof(vc->kbd_buf));
        vc->kbd_len     = 0;
        vc->kbd_execute = false;
    } else {
        /* Cooked type-ahead the program did not read - keep for Zeus. */
        vc->kbd_buf[vc->kbd_len] = '\0';
        bool has_nl = false;
        for (uint32_t i = 0; i < vc->kbd_len; i++)
            if (vc->kbd_buf[i] == '\n') { has_nl = true; break; }
        vc->kbd_execute = has_nl;
    }
    vc->raw_mode    = false;
    kbd_raw_used    = false;
    vc->kbd_cursor  = vc->kbd_len;
    vc->kbd_dirty   = false;
    /* Line-editing chords pressed while the program ran must not apply
     * to the next Zeus line (stale Home/Ctrl-U moved or cut its start). */
    arrow_up_pressed = arrow_down_pressed = false;
    arrow_left_pressed = arrow_right_pressed = false;
    home_pressed = end_pressed = false;
    kill_line_pressed = kill_word_pressed = false;
    tab_requested = false;
    escape_pressed = false;
    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
}

void keyboard_feed_char(char ch) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
    uint64_t irq_flags = irq_save();
    spinlock_lock(&keyboard_lock);

    if (vc->raw_mode) {
        if (vc->kbd_len < sizeof(vc->kbd_buf) - 1) {
            vc->kbd_buf[vc->kbd_len++] = ch;
        }
        spinlock_unlock(&keyboard_lock);
        irq_restore(irq_flags);
        return;
    }

    if (ch == '\n') {
        vc->kbd_cursor = vc->kbd_len;
        if (vc->kbd_len < sizeof(vc->kbd_buf) - 1)
            vc->kbd_buf[vc->kbd_len++] = ch;
        vc->kbd_buf[vc->kbd_len] = '\0';
        safe_echo_char(ch);
        vc->kbd_execute = true;
        spinlock_unlock(&keyboard_lock);
        irq_restore(irq_flags);
        return;
    }

    if (ch == '\b') {
        if (vc->kbd_cursor > 0 && vc->kbd_len > 0) {
            uint32_t cur = vc->kbd_cursor - 1;
            memmove(vc->kbd_buf + cur, vc->kbd_buf + cur + 1,
                    vc->kbd_len - cur - 1);
            vc->kbd_len--;
            vc->kbd_cursor = cur;
            vc->kbd_buf[vc->kbd_len] = '\0';
            if (vc->kbd_cursor == vc->kbd_len) {
                safe_echo_char('\b');
                safe_echo_char(' ');
                safe_echo_char('\b');
            } else {
                vc->kbd_dirty = true;
            }
        }
        spinlock_unlock(&keyboard_lock);
        irq_restore(irq_flags);
        return;
    }

    if (vc->kbd_len < sizeof(vc->kbd_buf) - 1) {
        uint32_t cur = vc->kbd_cursor;
        if (cur > vc->kbd_len) cur = vc->kbd_len;
        if (cur < vc->kbd_len)
            memmove(vc->kbd_buf + cur + 1, vc->kbd_buf + cur, vc->kbd_len - cur);
        vc->kbd_buf[cur] = ch;
        vc->kbd_len++;
        vc->kbd_cursor = cur + 1;
        vc->kbd_buf[vc->kbd_len] = '\0';
        if (vc->kbd_cursor == vc->kbd_len)
            safe_echo_char(ch);
        else
            vc->kbd_dirty = true;
    }

    spinlock_unlock(&keyboard_lock);
    irq_restore(irq_flags);
}

/* ================================================================
 * Keyboard layout management
 * ================================================================ */
void keyboard_set_layout(int layout_id) {
    if (layout_id >= 0 && layout_id < KB_LAYOUT_MAX) {
        current_layout = layout_id;
    }
}

int keyboard_get_layout(void) {
    return current_layout;
}

const char* keyboard_get_layout_name(void) {
    int l = current_layout;
    if (l < 0 || l >= KB_LAYOUT_MAX) l = KB_LAYOUT_US;
    return layout_names[l];
}

/* Modifier state for the desktop (pointer events carry Shift/Ctrl). */
bool keyboard_get_shift(void) { return shift_pressed; }
bool keyboard_get_ctrl(void)  { return left_ctrl_pressed; }
