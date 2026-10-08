#include "term.h"
#include "hpet.h"
#include "ktime.h"   /* ktime_now_ms() for console paint coalescing */
#include <stdarg.h>
#include "types.h"
#include "limine.h"
#include "mem.h"
#include "serial.h"
#include "atomic.h"
#include "string.h"
#include "stdio.h"  // For sprintf
#include "vmware_svga.h"
#include "amd_gpu.h"
#include "gpu_hal.h"
#include "virtio_gpu.h"
#include "mouse.h"
#include "vconsole.h"
#include "kernel_symtab.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define MAX_INT_SIZE 128

extern int snprintf(char *str, size_t size, const char *fmt, ...);
extern int kstrcmp(const char *s1, const char *s2);

// Function declarations for framebuffer functions
void fb_console_clear(void);
void fb_console_scroll(void);

// Flanterm protection - simple spinlock
static volatile int flanterm_lock = 0;
static volatile int interrupt_context = 0;
// Global flag to allow disabling flanterm permanently at runtime
volatile int flanterm_enabled = 1;

/* All per-session flags (terminate, force-kill, program-running) now live
 * inside each vconsole_t.  The functions below delegate to the VC that
 * belongs to the calling context (current process's cont_tty, falling back
 * to the active VC when called from interrupt / kernel-main context). */

bool get_terminate_flag(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    return vc->terminate_flag;
}

bool get_force_kill_flag(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    return vc->force_kill_flag;
}

bool get_program_running(void) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    return vc->program_running;
}

void set_program_running(bool value) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    vc->program_running = value;
}

void set_terminate_flag(bool value) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    vc->terminate_flag = value;
    if (!value) vc->force_kill_flag = false;
}

void set_force_kill_flag(bool value) {
    vconsole_t *vc = vconsole_get_by_id(vconsole_get_thread_console());
    vc->force_kill_flag = value;
}

// Simple atomic test-and-set for flanterm lock
static inline int flanterm_trylock(void) {
    int old = __sync_val_compare_and_swap(&flanterm_lock, 0, 1);
    return old == 0;  // Returns 1 if we got the lock, 0 if already locked
}

static inline void flanterm_unlock(void) {
    __sync_lock_release(&flanterm_lock);
}

// Functions to track interrupt context
void enter_interrupt_context(void) {
    __sync_fetch_and_add(&interrupt_context, 1);
}

void exit_interrupt_context(void) {
    __sync_fetch_and_sub(&interrupt_context, 1);
}

int in_interrupt_context(void) {
    return interrupt_context > 0;
}

#define FORMAT_LENTGH       1
#define NORMAL              0
#define STATE_SHORT         2
#define STATE_LONG          3
#define FORMAT_SPECIFIER    1
#define PERCENT_FORMAT      2

#define LENGTH_DEFAULT      0
#define LENGTH_SHORT_SHORT  1
#define LENGTH_SHORT        2
#define LENGTH_LONG         3
#define LENGTH_LONG_LONG    4


volatile struct limine_framebuffer_request fb_rq = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

struct limine_framebuffer *fb;

/* Physical/boot framebuffer limits (Limine mapping); logical fb->width/height may be smaller. */
static uint32_t fb_boot_max_w = 0;
static uint32_t fb_boot_max_h = 0;

static uint8_t  *backbuffer    = NULL;
static void     *real_fb_addr  = NULL;
static uint32_t  fb_size_bytes = 0;
static uint32_t  dirty_y_min   = 0xFFFFFFFF;
static uint32_t  dirty_y_max   = 0;

/* Console paint coalescing (see fb_flush): printf()s closer together than this
 * share one GPU transfer instead of paying one each. */
#define FB_FLUSH_COALESCE_MS 16
static bool     fb_defer_enabled  = false;
static bool     fb_flush_waiting  = false;
static uint64_t fb_last_paint_ms  = 0;

/* Scroll batching.
 *
 * Scrolling the screen one text row costs a framebuffer memmove of almost the
 * whole screen *plus* a copy of the same size back to the real framebuffer, so
 * a command that prints a full screen of text (`ls /` = 54 lines) spent over a
 * second just moving pixels.  The chargrid already holds the true screen
 * contents, so when several lines scroll before the screen is repainted we can
 * skip the per-line moves entirely and let one repaint from the chargrid cover
 * all of them.  The first scroll in a burst still uses the cheap direct move,
 * so single lines (prompts, status output) behave exactly as before. */
static bool     console_repaint_pending    = false;
static uint32_t console_scrolls_since_paint = 0;

static void chargrid_repaint_all(void);
static void fb_draw_cursor(bool show);

static inline void fb_mark_dirty(uint32_t y, uint32_t h) {
    if (y < dirty_y_min) dirty_y_min = y;
    uint32_t y_end = y + h;
    if (fb && y_end > fb->height) y_end = fb->height;
    if (y_end > dirty_y_max) dirty_y_max = y_end;
}

/* When virtio-gpu owns scanout, copy dirty Limine/backbuffer rows into the
 * virtio backing store and flush once (batched).  Console drawing stays on
 * the Limine FB; user apps use fb_app_* syscalls instead. */
static void fb_sync_virtio_scanout(uint32_t ymin, uint32_t ymax) {
    if (!fb || !virtio_gpu_is_available()) return;

    gpu_surface_t *surf = gpu_hal_get_primary_surface();
    if (!surf || !surf->pixels) return;
    if (ymin >= ymax || ymin >= (uint32_t)fb->height) return;
    if (ymax > (uint32_t)fb->height) ymax = (uint32_t)fb->height;

    uint32_t copy_w = (uint32_t)fb->width;
    uint32_t vw = virtio_gpu_get_width();
    if (copy_w > vw) copy_w = vw;

    /* Limine FB is the virtio backing store — VGA already shows it. */
    if ((void *)surf->pixels == (void *)fb->address || virtio_gpu_using_live_vga()) {
        return;
    }

    const uint8_t *src = (const uint8_t *)(backbuffer && (void *)backbuffer != real_fb_addr
                                            ? backbuffer : fb->address);
    uint32_t *dst = surf->pixels;

    for (uint32_t row = ymin; row < ymax; row++) {
        memcpy(dst + (uint64_t)row * vw,
               src + (uint64_t)row * fb->pitch,
               (size_t)copy_w * sizeof(uint32_t));
    }
    gpu_hal_present_rect(0, ymin, copy_w, ymax - ymin);
}

void fb_flush(void) {
    if (!fb) return;
    if (dirty_y_min >= dirty_y_max) return;

    /* Console output batching.
     *
     * Every paint is a memcpy plus a GPU scanout transfer of the dirty rows;
     * a program that prints many lines (ls, help, cat) used to pay one
     * transfer per line.  Paints closer together than the window below are
     * merged into the next one, which is painted by the kernel main loop (see
     * fb_flush_pending) or by the next fb_flush() from the printing thread.
     * The window is deliberately short: a program that blocks the main loop
     * while prompting (stopwatch, a y/n question) must still get its output on
     * screen without waiting for the program to exit. */
    if (fb_defer_enabled) {
        uint64_t now = ktime_now_ms();
        if (fb_last_paint_ms && now - fb_last_paint_ms < FB_FLUSH_COALESCE_MS) {
            fb_flush_waiting = true;
            return;
        }
        fb_last_paint_ms = now;
        fb_flush_waiting = false;
    }

    /* A burst of scrolls skipped its per-line pixel moves; rebuild those rows
     * from the chargrid now, before the dirty rows are copied out. */
    if (console_repaint_pending) {
        console_repaint_pending = false;
        chargrid_repaint_all();
    }
    console_scrolls_since_paint = 0;

    uint32_t ymin = dirty_y_min;
    uint32_t ymax = dirty_y_max;
    if (ymin >= (uint32_t)fb->height) {
        dirty_y_min = 0xFFFFFFFF;
        dirty_y_max = 0;
        return;
    }
    if (ymax > (uint32_t)fb->height) ymax = (uint32_t)fb->height;

    if (backbuffer && real_fb_addr && (void *)backbuffer != real_fb_addr) {
        uint32_t start = ymin * fb->pitch;
        uint32_t end   = ymax * fb->pitch;
        memcpy((uint8_t *)real_fb_addr + start, backbuffer + start, end - start);
    }

    fb_sync_virtio_scanout(ymin, ymax);

    dirty_y_min = 0xFFFFFFFF;
    dirty_y_max = 0;
}

/* Enable/disable the coalescing window above (the kernel main loop turns it on
 * once it is running and can call fb_flush_pending() regularly). */
void fb_set_defer_flush(bool enable) {
    fb_defer_enabled = enable;
}

/* Paint anything the coalescing window is holding back.  Cheap when idle. */
void fb_flush_pending(void) {
    if (fb_flush_waiting) fb_flush();
}

/* Push the full virtio scanout after a Limine→virtio sync (console redraw, etc.). */
void fb_present_scanout(void) {
    if (!virtio_gpu_is_available() || !fb) return;
    if (virtio_gpu_using_live_vga()) return;
    gpu_surface_t *surf = gpu_hal_get_primary_surface();
    if (!surf || !surf->pixels) return;
    gpu_hal_present_all();
}

void fb_mark_dirty_all(void) {
    if (!fb) return;
    dirty_y_min = 0;
    dirty_y_max = fb->height;
}

/* Public row-range dirty marking so the desktop compositor can flush only
 * the damaged scanlines instead of the full screen. */
void fb_mark_dirty_rows(uint32_t y, uint32_t h) {
    fb_mark_dirty(y, h);
}

const void *fb_get_composite_buffer(void) {
    if (!fb)
        return NULL;
    if (backbuffer)
        return backbuffer;
    return fb->address;
}

/* flanterm framebuffer code removed */

/* legacy custom printf removed */

void putpixel(uint64_t x, uint64_t y, uint32_t color){
    if (!fb) return;
    if (x >= fb->width || y >= fb->height) return;

    if (amd_gpu_is_available()) {
        amd_gpu_fillrect(color, x, y, 1, 1);
        return;
    }

    uint8_t *target = backbuffer ? backbuffer : (uint8_t*)fb->address;

    if (fb->bpp == 32) {
        ((uint32_t*)target)[y * (fb->pitch / 4) + x] = color;
    } else if (fb->bpp == 24) {
        uint64_t offset = y * fb->pitch + x * 3;
        target[offset]     = color & 0xFF;
        target[offset + 1] = (color >> 8) & 0xFF;
        target[offset + 2] = (color >> 16) & 0xFF;
    } else {
        ((uint32_t*)target)[y * (fb->pitch / 4) + x] = color;
    }
    fb_mark_dirty(y, 1);
}

void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h){
    if (!fb) return;
    if (x >= fb->width || y >= fb->height) return;
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;

    if (vmware_svga_is_available() && (w * h) >= 16) {
        vmware_svga_fillrect(color, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
        return;
    }
    if (amd_gpu_is_available()) {
        amd_gpu_fillrect(color, x, y, w, h);
        return;
    }

    uint8_t *target = backbuffer ? backbuffer : (uint8_t*)fb->address;

    if (fb->bpp == 32) {
        uint32_t *screen = (uint32_t*)target;
        uint32_t ppl = fb->pitch / 4;
        for (uint64_t j = 0; j < h; j++) {
            uint32_t *row = screen + (y + j) * ppl + x;
            for (uint64_t i = 0; i < w; i++)
                row[i] = color;
        }
    } else {
        for (uint64_t j = 0; j < h; j++)
            for (uint64_t i = 0; i < w; i++)
                putpixel(x + i, y + j, color);
    }
    fb_mark_dirty(y, h);
}

uint32_t get_pixel_color(uint8_t pixel){
    uint32_t color = pixel;
    color |= pixel << 8;
    color |= pixel << 16;
    return color;
}

void printch(void* ctx,char c){ (void)ctx; term_putchar(c);}

void printstr(void* ctx,char *str){ (void)ctx; if(str) term_write(str,strlen(str)); }

void printk(void* ctx, char* str){ (void)ctx; term_write("[KERNEL] ",9); term_write(str,strlen(str)); }

void printex(void* ctx, char* str){ (void)ctx; term_write("[EXCEPTION] ",12); term_write(str,strlen(str)); }

/* binary/int/hex helpers obsolete with kprintf */

void log_panic(char *msg){
    printf("%skern_panic: %s%s\n", ANSI_COLOR_RED, ANSI_COLOR_RESET, msg);
    __asm__ volatile ("cli"); 
    __asm__ volatile ("hlt");
}

void log_err(char *msg){
    printf("%skern_err: %s%s\n", ANSI_COLOR_RED, ANSI_COLOR_RESET, msg);

}

void log_warn(char *msg){
    printf("%skern_warn: %s%s\n", ANSI_COLOR_YELLOW, ANSI_COLOR_RESET, msg);
}

void log_info(char *msg){
    printf("%skern_info: %s%s\n", ANSI_COLOR_CYAN, ANSI_COLOR_RESET, msg);
}

void log_success(char *msg){
    printf("%skern_success: %s%s\n", ANSI_COLOR_GREEN, ANSI_COLOR_RESET, msg);
}

char* int2string(uint32_t value) {
    static char buf[32] = {0};
    int i = 30;
    if (value == 0) {
        buf[30] = '0';
        return &buf[30];
    }
    for(; value && i ; --i, value /= 10) {
        buf[i] = "0123456789"[value % 10];
    }
    return &buf[i+1];
}

char* int2hex(uint64_t value) {
    static char buf[64] = {0};
    int i = 62;
    if (value == 0) {
        buf[62] = '0';
        return &buf[62];
    }
    for(; value && i ; --i, value /= 16) {
        buf[i] = "0123456789ABCDEF"[value % 16];
    }
    return &buf[i+1];
}


char* strtok(char* str, const char* delim) {
    static char* pos = NULL;
    if (str) pos = str;           // Reset static position when new string passed
    if (!pos) return NULL;

    /* Skip any leading delimiters so we don’t return empty tokens */
    pos += strspn(pos, delim);
    if (*pos == '\0') {
        return NULL;              // Reached end after skipping delimiters
    }

    /* Find length until next delimiter */
    size_t len = strcspn(pos, delim);
    char *token = pos;
    pos += len;
    if (*pos != '\0') {
        *pos++ = '\0';           // Null-terminate token and advance past delimiter
    }
    return token;
}

/* Use compiler built-ins from <string.h> instead of local versions */

char* remove_nl(char* str){
    if (str == NULL) {
        return NULL;
    }
    uint32_t str_len = strlen(str);
    for(uint32_t i = 0; i < str_len; i++) {
        if(str[i] == '\n'){
            str[i] = '\0';
        }
    }
    return str;
}

bool strEql(const char* str1, const char* str2) {
    if (str1 == NULL || str2 == NULL) {
        return (str1 == str2); // Both NULL = equal, one NULL = not equal
    }
    uint32_t str1_len = strlen(str1);
    uint32_t str2_len = strlen(str2);
    if (str1_len != str2_len) {
        return false;
    }
    for(uint32_t i = 0; i < str1_len; i++) {
        if(str1[i] != str2[i]) {
            return false;
        }
    }
    return true;
}

void dump_hex(char *stack) {
    for (int i = 0; i < 64; i++) {
        printf("%02x ", (unsigned char)stack[i]);
    }
    printf("\n");
}

void dump_str(char *stack) {
    int i = 0;
    while (i < 64) {
        printf("%s", &stack[i]);
        while (i < 64 && stack[i]) i++;
        i++;
    }
    printf("\n");
}

void wait(uint64_t ms){
    hpet_sleep_counter(ms);
}
/* Canary for -fstack-protector (-mstack-protector-guard=global).  The low
 * byte stays zero so string overflows can't reproduce it. */
uint64_t __stack_chk_guard = 0x595e9fbd94fda700ULL;

/* Must not itself be protected: frames live across the guard change would
 * fail their check on return.  Call from kmain(), which never returns. */
__attribute__((no_stack_protector, noinline))
void stack_guard_init(void) {
    uint64_t seed = 0;
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & (1u << 30)) {
        for (int i = 0; i < 10; i++) {
            unsigned char ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(seed), "=qm"(ok) :: "cc");
            if (ok) break;
            seed = 0;
        }
    }
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    seed ^= ((uint64_t)hi << 32 | lo) * 0x9E3779B97F4A7C15ULL;
    seed ^= (uint64_t)(uintptr_t)&seed;
    __stack_chk_guard = (seed ^ __stack_chk_guard) & ~0xFFULL;
}

__attribute__((no_stack_protector, noreturn))
void __stack_chk_fail(void) {
    uint64_t ra = (uint64_t)__builtin_return_address(0);
    serial_printf("\n[ssp] stack smashing detected in %s (return 0x%llx)\n",
                  lookup_symbol(ra), (unsigned long long)ra);
    log_panic("Stack smashing detected");
    for (;;)
        __asm__ volatile("cli; hlt");
}

const char* get_active_gpu_driver(void) {
    gpu_device_t *dev = gpu_hal_get_active();
    if (dev && dev->ops)
        return dev->ops->name;
    if (vmware_svga_is_available())
        return "VMware SVGA-II";
    if (amd_gpu_is_available())
        return "AMD GPU";
    return "Direct Framebuffer (No GPU acceleration)";
}

bool is_gpu_acceleration_available(void) {
    uint32_t caps = gpu_hal_get_capabilities();
    if (caps & GPU_CAP_2D)
        return true;
    return vmware_svga_is_available() || amd_gpu_is_available();
}

void clear_screen(){
    // Use SVGA acceleration for screen clearing (lazy initialization)
    if (vmware_svga_is_available()) {
        vmware_svga_clear_screen(0x00000000);
        return;
    }
    
    if (amd_gpu_is_available()) {
        amd_gpu_clear_screen(0x00000000);
        return;
    }
    
    // Fallback to framebuffer console clear
    fb_console_clear();
}

/* Ring-3 framebuffer syscalls (dvd.exe, etc.) — draw on the active scanout
 * backend directly instead of the Limine linear framebuffer. */
void fb_app_clear(void) {
    if (gpu_hal_get_capabilities() & GPU_CAP_2D) {
        gpu_hal_clear(0x00000000);
        if (!virtio_gpu_using_live_vga())
            gpu_hal_present_all();
        return;
    }
    clear_screen();
}

void fb_app_fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (gpu_hal_get_capabilities() & GPU_CAP_2D) {
        gpu_hal_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, color);
        if (!virtio_gpu_using_live_vga())
            gpu_hal_present_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
        return;
    }
    fillrect(color, x, y, w, h);
    fb_mark_dirty_rows((uint32_t)y, (uint32_t)h);
    fb_flush();
}

// VGA backend: no flanterm context needed

/* Framebuffer Console Implementation */

// High-quality 16x8 bitmap font data from flanterm (ASCII 32-127)
const uint8_t font_16x8[128][16] = {
    // Space (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ! (33)
    {0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},
    // " (34)
    {0x00, 0x00, 0x00, 0x6C, 0x6C, 0x6C, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // # (35)
    {0x00, 0x00, 0x00, 0x6C, 0x6C, 0x6C, 0xFE, 0x6C, 0x6C, 0x6C, 0xFE, 0x6C, 0x6C, 0x6C, 0x00, 0x00},
    // $ (36)
    {0x00, 0x00, 0x18, 0x18, 0x7C, 0xC6, 0xC0, 0xC0, 0x7C, 0x06, 0x06, 0xC6, 0x7C, 0x18, 0x18, 0x00},
    // % (37)
    {0x00, 0x00, 0x00, 0x00, 0xC2, 0xC6, 0x0C, 0x18, 0x30, 0x60, 0xC6, 0x86, 0x00, 0x00, 0x00, 0x00},
    // & (38)
    {0x00, 0x00, 0x00, 0x38, 0x6C, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00},
    // ' (39)
    {0x00, 0x00, 0x00, 0x30, 0x30, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ( (40)
    {0x00, 0x00, 0x00, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00},
    // ) (41)
    {0x00, 0x00, 0x00, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00, 0x00, 0x00},
    // * (42)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // + (43)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // , (44)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00, 0x00},
    // - (45)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // . (46)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00},
    // / (47)
    {0x00, 0x00, 0x00, 0x06, 0x06, 0x0C, 0x0C, 0x18, 0x18, 0x30, 0x30, 0x60, 0x60, 0xC0, 0xC0, 0x00},
    // 0 (48)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // 1 (49)
    {0x00, 0x00, 0x00, 0x18, 0x38, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00},
    // 2 (50)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0x06, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC6, 0xFE, 0x00, 0x00, 0x00},
    // 3 (51)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0x06, 0x06, 0x3C, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // 4 (52)
    {0x00, 0x00, 0x00, 0x0C, 0x1C, 0x3C, 0x6C, 0xCC, 0xCC, 0xFE, 0x0C, 0x0C, 0x0C, 0x00, 0x00, 0x00},
    // 5 (53)
    {0x00, 0x00, 0x00, 0xFE, 0xC0, 0xC0, 0xC0, 0xFC, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // 6 (54)
    {0x00, 0x00, 0x00, 0x3C, 0x60, 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // 7 (55)
    {0x00, 0x00, 0x00, 0xFE, 0xC6, 0x06, 0x06, 0x0C, 0x18, 0x18, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00},
    // 8 (56)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // 9 (57)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0x7E, 0x06, 0x06, 0x0C, 0x78, 0x00, 0x00, 0x00},
    // : (58)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00},
    // ; (59)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00, 0x00},
    // < (60)
    {0x00, 0x00, 0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00, 0x00, 0x00, 0x00},
    // = (61)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // > (62)
    {0x00, 0x00, 0x00, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00},
    // ? (63)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0x0C, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00},
    // @ (64)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xDE, 0xDE, 0xDE, 0xDC, 0xC0, 0xC0, 0x7C, 0x00, 0x00, 0x00},
    // A (65)
    {0x00, 0x00, 0x00, 0x18, 0x3C, 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00},
    // B (66)
    {0x00, 0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x66, 0x66, 0xFC, 0x00, 0x00, 0x00},
    // C (67)
    {0x00, 0x00, 0x00, 0x3C, 0x66, 0xC6, 0xC0, 0xC0, 0xC0, 0xC0, 0xC6, 0x66, 0x3C, 0x00, 0x00, 0x00},
    // D (68)
    {0x00, 0x00, 0x00, 0xF8, 0x6C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00, 0x00, 0x00},
    // E (69)
    {0x00, 0x00, 0x00, 0xFE, 0x66, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x66, 0xFE, 0x00, 0x00, 0x00},
    // F (70)
    {0x00, 0x00, 0x00, 0xFE, 0x66, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00},
    // G (71)
    {0x00, 0x00, 0x00, 0x3C, 0x66, 0xC6, 0xC0, 0xC0, 0xCE, 0xC6, 0xC6, 0x66, 0x3E, 0x00, 0x00, 0x00},
    // H (72)
    {0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00},
    // I (73)
    {0x00, 0x00, 0x00, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00},
    // J (74)
    {0x00, 0x00, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x4C, 0x4C, 0x38, 0x00, 0x00, 0x00},
    // K (75)
    {0x00, 0x00, 0x00, 0x66, 0x66, 0x6C, 0x6C, 0x78, 0x78, 0x6C, 0x6C, 0x66, 0x66, 0x00, 0x00, 0x00},
    // L (76)
    {0x00, 0x00, 0x00, 0xF0, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00, 0x00, 0x00},
    // M (77)
    {0x00, 0x00, 0x00, 0xC6, 0xC6, 0xEE, 0xEE, 0xFE, 0xD6, 0xD6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00},
    // N (78)
    {0x00, 0x00, 0x00, 0xC6, 0xC6, 0xE6, 0xE6, 0xF6, 0xFE, 0xDE, 0xCE, 0xC6, 0xC6, 0x00, 0x00, 0x00},
    // O (79)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // P (80)
    {0x00, 0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00},
    // Q (81)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xD6, 0xD6, 0x7C, 0x0C, 0x0E, 0x00, 0x00},
    // R (82)
    {0x00, 0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x66, 0xE6, 0x00, 0x00, 0x00},
    // S (83)
    {0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0x60, 0x38, 0x0C, 0x06, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // T (84)
    {0x00, 0x00, 0x00, 0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00},
    // U (85)
    {0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00},
    // V (86)
    {0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00, 0x00, 0x00},
    // W (87)
    {0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xD6, 0xD6, 0xFE, 0xEE, 0xC6, 0xC6, 0x00, 0x00, 0x00},
    // X (88)
    {0x00, 0x00, 0x00, 0xC6, 0xC6, 0x6C, 0x6C, 0x38, 0x38, 0x6C, 0x6C, 0xC6, 0xC6, 0x00, 0x00, 0x00},
    // Y (89)
    {0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00},
    // Z (90)
    {0x00, 0x00, 0x00, 0xFE, 0xC6, 0x86, 0x0C, 0x18, 0x30, 0x60, 0xC2, 0xC6, 0xFE, 0x00, 0x00, 0x00},
    // [ (91)
    {0x00, 0x00, 0x00, 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00, 0x00, 0x00},
    // \ (92)
    {0x00, 0x00, 0x00, 0xC0, 0xC0, 0x60, 0x60, 0x30, 0x30, 0x18, 0x18, 0x0C, 0x0C, 0x06, 0x06, 0x00},
    // ] (93)
    {0x00, 0x00, 0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00, 0x00, 0x00},
    // ^ (94)
    {0x00, 0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // _ (95)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    // ` (96)
    {0x00, 0x00, 0x00, 0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // a (97)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x06, 0x06, 0x7E, 0xC6, 0xC6, 0xC6, 0x7E, 0x00, 0x00, 0x00},
    // b (98)
    {0x00, 0x00, 0x00, 0xE0, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00},
    // c (99)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC0, 0xC0, 0xC0, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // d (100)
    {0x00, 0x00, 0x00, 0x1C, 0x0C, 0x0C, 0x7C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x7E, 0x00, 0x00, 0x00},
    // e (101)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xFE, 0xC0, 0xC0, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // f (102)
    {0x00, 0x00, 0x00, 0x38, 0x6C, 0x64, 0x60, 0xF0, 0x60, 0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00},
    // g (103)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0xCC, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xCC, 0x78, 0x00, 0x00},
    // h (104)
    {0x00, 0x00, 0x00, 0xE0, 0x60, 0x60, 0x6C, 0x76, 0x66, 0x66, 0x66, 0x66, 0xE6, 0x00, 0x00, 0x00},
    // i (105)
    {0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00},
    // j (106)
    {0x00, 0x00, 0x00, 0x06, 0x06, 0x00, 0x0E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x66, 0x66, 0x3C, 0x00},
    // k (107)
    {0x00, 0x00, 0x00, 0xE0, 0x60, 0x60, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0xE6, 0x00, 0x00, 0x00},
    // l (108)
    {0x00, 0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00},
    // m (109)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xEC, 0xFE, 0xD6, 0xD6, 0xD6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00},
    // n (110)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0x00, 0x00},
    // o (111)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // p (112)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00},
    // q (113)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0x0C, 0x1E, 0x00},
    // r (114)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xDE, 0x76, 0x66, 0x60, 0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00},
    // s (115)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0x60, 0x38, 0x0C, 0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00},
    // t (116)
    {0x00, 0x00, 0x00, 0x20, 0x60, 0x60, 0xFC, 0x60, 0x60, 0x60, 0x60, 0x64, 0x38, 0x00, 0x00, 0x00},
    // u (117)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x7E, 0x00, 0x00, 0x00},
    // v (118)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00, 0x00, 0x00},
    // w (119)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xD6, 0xD6, 0xD6, 0xFE, 0x6C, 0x00, 0x00, 0x00},
    // x (120)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x6C, 0xC6, 0xC6, 0x00, 0x00, 0x00},
    // y (121)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xCC, 0x78, 0x00, 0x00},
    // z (122)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xCC, 0x98, 0x30, 0x60, 0xC4, 0xCC, 0xFC, 0x00, 0x00, 0x00},
    // { (123)
    {0x00, 0x00, 0x00, 0x1C, 0x30, 0x30, 0x30, 0x60, 0x30, 0x30, 0x30, 0x30, 0x1C, 0x00, 0x00, 0x00},
    // | (124)
    {0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00},
    // } (125)
    {0x00, 0x00, 0x00, 0xE0, 0x30, 0x30, 0x30, 0x18, 0x30, 0x30, 0x30, 0x30, 0xE0, 0x00, 0x00, 0x00},
    // ~ (126)
    {0x00, 0x00, 0x00, 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

// Font is now handled by the unified 16x8 font above

// Pre-computed byte-reverse LUT (populated at runtime)
// (bit reversal no longer needed)

// Console dimensions and state
static uint32_t console_x = 0;
static uint32_t console_y = 0;
static uint32_t console_width = 80;  // Default characters per line
static uint32_t console_height = 25; // Default lines
static const uint32_t char_base = 8; // base glyph width in font table
static uint32_t font_scale = 1;      // 1 = 8x16, 2 = 16x32 etc.
static uint32_t char_width  = 8;
static uint32_t char_height = 16;  // Base font height

/* ================================================================
 * ANSI escape sequence parser state
 * ================================================================ */
#define ANSI_STATE_NORMAL    0
#define ANSI_STATE_ESC       1  /* Received ESC (0x1B) */
#define ANSI_STATE_CSI       2  /* Received ESC [ */
#define ANSI_STATE_OSC       3  /* Received ESC ] (Operating System Command) */
#define ANSI_STATE_DEC       4  /* Received ESC [ ? (DEC private mode) */
#define ANSI_MAX_PARAMS      16 /* Increased for 256-color/truecolor */
static int ansi_state = ANSI_STATE_NORMAL;
static int ansi_params[ANSI_MAX_PARAMS];
static int ansi_param_count = 0;
static int ansi_cur_param = 0;  /* Accumulator for current numeric param */
static bool ansi_param_started = false;
static bool ansi_dec_private = false; /* ESC [ ? ... mode */

/* Text attributes bitmask */
#define ATTR_BOLD        (1 << 0)
#define ATTR_DIM         (1 << 1)
#define ATTR_ITALIC      (1 << 2)
#define ATTR_UNDERLINE   (1 << 3)
#define ATTR_BLINK       (1 << 4)
#define ATTR_INVERSE     (1 << 5)
#define ATTR_HIDDEN      (1 << 6)
#define ATTR_STRIKETHROUGH (1 << 7)
static uint32_t text_attrs = 0;

/* Saved cursor position (ESC[s / ESC[u and ESC 7 / ESC 8) */
static uint32_t saved_cursor_x = 0;
static uint32_t saved_cursor_y = 0;
static uint32_t saved_fg = 0xFFFFFF;
static uint32_t saved_bg = 0x000000;
static uint32_t saved_attrs = 0;

/* Cursor visibility (ESC[?25h / ESC[?25l) */
static bool cursor_enabled = true;

/* Scroll region (top and bottom, 0-based inclusive) */
static uint32_t scroll_top = 0;
static uint32_t scroll_bottom = 0; /* 0 = use console_height-1 */

/* Alternate screen buffer (ESC[?1049h / ESC[?1049l) */
#define ALT_CHARGRID_MAX_ROWS 128
#define ALT_CHARGRID_MAX_COLS 256
static char     alt_chargrid[ALT_CHARGRID_MAX_ROWS][ALT_CHARGRID_MAX_COLS];
static uint32_t alt_chargrid_fg[ALT_CHARGRID_MAX_ROWS][ALT_CHARGRID_MAX_COLS];
static uint32_t alt_chargrid_bg[ALT_CHARGRID_MAX_ROWS][ALT_CHARGRID_MAX_COLS];
static uint32_t alt_cursor_x = 0, alt_cursor_y = 0;
static bool     alt_screen_active = false;

/* Tab stops (every 8 columns by default) */
#define TAB_STOP_SIZE 8

/* Standard ANSI 4-bit colour palette (indices 0-7 normal, 8-15 bright) */
static const uint32_t ansi_color_table[16] = {
    0x000000, /* 0 black */
    0xAA0000, /* 1 red */
    0x00AA00, /* 2 green */
    0xAA5500, /* 3 yellow/brown */
    0x0000AA, /* 4 blue */
    0xAA00AA, /* 5 magenta */
    0x00AAAA, /* 6 cyan */
    0xAAAAAA, /* 7 white (light grey) */
    0x555555, /* 8 bright black (dark grey) */
    0xFF5555, /* 9 bright red */
    0x55FF55, /* 10 bright green */
    0xFFFF55, /* 11 bright yellow */
    0x5555FF, /* 12 bright blue */
    0xFF55FF, /* 13 bright magenta */
    0x55FFFF, /* 14 bright cyan */
    0xFFFFFF, /* 15 bright white */
};

/* Saved fg/bg for reset (SGR 0) */
static uint32_t default_fg = 0xFFFFFF;
static uint32_t default_bg = 0x000000;

/* Forward declarations for functions defined later in this file */
static void ansi_handle_csi(char final_char);
static void ansi_handle_sgr(void);
static void ansi_handle_dec_mode(char final_char);
static void fb_draw_char_at(uint32_t px, uint32_t py, char c,
                            uint32_t fg, uint32_t bg_c);
static void fb_redraw_chargrid_line(uint32_t scr_row);
void fb_console_redraw_with_scrollback(void);
static void fb_scroll_region_up(uint32_t top, uint32_t bottom);
static void fb_scroll_region_down(uint32_t top, uint32_t bottom);

/* Convert 256-color index to RGB */
static uint32_t ansi_256_to_rgb(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    /* 0-15: standard + bright colors */
    if (idx < 16) return ansi_color_table[idx];
    /* 16-231: 6x6x6 color cube */
    if (idx < 232) {
        int n = idx - 16;
        int b = n % 6;
        int g = (n / 6) % 6;
        int r = n / 36;
        /* Map 0-5 to 0x00,0x5F,0x87,0xAF,0xD7,0xFF */
        static const uint8_t cube[] = {0x00, 0x5F, 0x87, 0xAF, 0xD7, 0xFF};
        return ((uint32_t)cube[r] << 16) | ((uint32_t)cube[g] << 8) | (uint32_t)cube[b];
    }
    /* 232-255: grayscale ramp (08, 12, ... e8) */
    uint8_t grey = (uint8_t)(8 + (idx - 232) * 10);
    return ((uint32_t)grey << 16) | ((uint32_t)grey << 8) | (uint32_t)grey;
}

/* ================================================================
 * Scrollback buffer
 * ================================================================ */
#define SCROLLBACK_LINES 200
#define SCROLLBACK_COLS_MAX 256
static char scrollback_buf[SCROLLBACK_LINES][SCROLLBACK_COLS_MAX];
static uint32_t scrollback_fg_buf[SCROLLBACK_LINES][SCROLLBACK_COLS_MAX];
static uint32_t scrollback_write = 0;   /* Next line to write (ring index) */
static uint32_t scrollback_count = 0;   /* Total lines stored (max SCROLLBACK_LINES) */
static int scrollback_offset = 0;       /* 0 = live view, >0 = scrolled back N lines */

/* (scrollback_store_screen_line is used below to store lines into scrollback) */

/* ================================================================
 * Character-grid buffer for scrollback reconstruction
 * ================================================================ */
#define CHARGRID_MAX_COLS 256
#define CHARGRID_MAX_ROWS 128
static char chargrid[CHARGRID_MAX_ROWS][CHARGRID_MAX_COLS];
static uint32_t chargrid_fg[CHARGRID_MAX_ROWS][CHARGRID_MAX_COLS];
static uint32_t chargrid_bg_arr[CHARGRID_MAX_ROWS][CHARGRID_MAX_COLS];
static bool chargrid_initialized = false;

static void chargrid_init(void) {
    if (chargrid_initialized) return;
    for (int r = 0; r < CHARGRID_MAX_ROWS; r++) {
        for (int c = 0; c < CHARGRID_MAX_COLS; c++) {
            chargrid[r][c] = ' ';
            chargrid_fg[r][c] = 0xFFFFFF;
            chargrid_bg_arr[r][c] = 0x000000;
        }
    }
    chargrid_initialized = true;
}

/* Store current screen line into scrollback when scrolling */
static void scrollback_store_screen_line(uint32_t row) {
    if (row >= CHARGRID_MAX_ROWS || !chargrid_initialized) return;
    uint32_t idx = scrollback_write % SCROLLBACK_LINES;
    uint32_t cols = console_width;
    if (cols > SCROLLBACK_COLS_MAX) cols = SCROLLBACK_COLS_MAX;
    for (uint32_t c = 0; c < cols; c++) {
        scrollback_buf[idx][c] = chargrid[row][c];
        scrollback_fg_buf[idx][c] = chargrid_fg[row][c];
    }
    /* NUL-terminate if space */
    if (cols < SCROLLBACK_COLS_MAX) scrollback_buf[idx][cols] = '\0';
    scrollback_write++;
    if (scrollback_count < SCROLLBACK_LINES) scrollback_count++;
}

/* Shift the character grid up by one row */
static void chargrid_scroll_up(void) {
    if (!chargrid_initialized) return;
    uint32_t rows = console_height;
    if (rows > CHARGRID_MAX_ROWS) rows = CHARGRID_MAX_ROWS;
    uint32_t cols = console_width;
    if (cols > CHARGRID_MAX_COLS) cols = CHARGRID_MAX_COLS;
    for (uint32_t r = 1; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            chargrid[r-1][c] = chargrid[r][c];
            chargrid_fg[r-1][c] = chargrid_fg[r][c];
            chargrid_bg_arr[r-1][c] = chargrid_bg_arr[r][c];
        }
    }
    /* Clear last row */
    uint32_t last = rows - 1;
    for (uint32_t c = 0; c < cols; c++) {
        chargrid[last][c] = ' ';
        chargrid_fg[last][c] = 0xFFFFFF;
        chargrid_bg_arr[last][c] = 0x000000;
    }
}

void recalc_console_dims(void){
    char_width  = char_base * font_scale;
    char_height = 16 * font_scale;  // Base font height is 16 pixels
    if(fb){
        console_width  = fb->width  / char_width;
        console_height = fb->height / char_height;
    }
}

uint32_t console_get_width(void){
    return console_width;
}

uint32_t console_get_height(void){
    return console_height;
}

// Framebuffer pixel dimensions for graphics programming
uint32_t fb_get_width(void){
    if (!fb) return 0;
    return (uint32_t)fb->width;
}

uint32_t fb_get_height(void){
    if (!fb) return 0;
    return (uint32_t)fb->height;
}

uint32_t fb_max_width(void)
{
    if (!fb)
        return 0;
    if (fb_boot_max_w == 0)
        return (uint32_t)fb->width;
    uint32_t mx = fb_boot_max_w;
    if (fb->bpp == 32) {
        uint32_t stride_px = (uint32_t)(fb->pitch / 4u);
        if (stride_px < mx)
            mx = stride_px;
    }
    return mx;
}

uint32_t fb_max_height(void)
{
    if (!fb)
        return 0;
    return fb_boot_max_h ? fb_boot_max_h : (uint32_t)fb->height;
}

int fb_set_video_resolution(uint32_t w, uint32_t h)
{
    if (!fb)
        return -1;
    if (fb_boot_max_w == 0) {
        fb_boot_max_w = (uint32_t)fb->width;
        fb_boot_max_h = (uint32_t)fb->height;
    }
    uint32_t max_w = fb_max_width();
    uint32_t max_h = fb_max_height();
    if (w == 0 || h == 0 || w > max_w || h > max_h)
        return -1;

    gpu_device_t *adev = gpu_hal_get_active();
    if (virtio_gpu_is_available() && adev && adev->ops &&
        kstrcmp(adev->ops->name, "virtio-gpu") == 0) {
        if (virtio_gpu_set_viewport_size(w, h) < 0)
            return -1;
    }

    fb->width  = w;
    fb->height = h;

    recalc_console_dims();
    fb_console_clear();
    mouse_sync_screen_bounds();
    fb_flush();
    return 0;
}

void fb_set_font_scale(uint32_t scale){
    if(scale == 0 || scale > 4) return;
    font_scale = scale;
    recalc_console_dims();
    fb_console_clear();
    console_x = console_y = 0;
}

static uint32_t fg_color = 0xFFFFFF; // White
static uint32_t bg_color = 0x000000; // Black

void console_set_fg(uint32_t color){
    fg_color = color;
}

uint32_t console_get_fg(void){
    return fg_color;
}

// Test function to verify framebuffer works
void fb_test_pattern(void) {
    if (!fb) {
        serial_puts("[fb_test] No framebuffer available\n");
        return;
    }
    
    serial_puts("[fb_test] Starting simple pixel test\n");
    
    // Test just a few individual pixels
    putpixel(0, 0, 0xFF0000);      // Red pixel at top-left
    putpixel(1, 0, 0x00FF00);      // Green pixel 
    putpixel(2, 0, 0x0000FF);      // Blue pixel
    putpixel(3, 0, 0xFFFFFF);      // White pixel
    
    serial_puts("[fb_test] Individual pixels set\n");
    
    // Test a small 10x10 rectangle
    for(int y = 10; y < 20; y++) {
        for(int x = 10; x < 20; x++) {
            putpixel(x, y, 0xFFFF00);  // Yellow square
        }
    }
    
    serial_puts("[fb_test] Small rectangle drawn\n");
    serial_puts("[fb_test] Basic pixel test completed\n");
}

void fb_console_init(void) {
    if (!fb) {
        serial_puts("[fb_console] Framebuffer not available!\n");
        return;
    }

    
    // Log actual framebuffer details for debugging
    serial_printf("[fb_console] FB address: %p\n", fb->address);
    serial_printf("[fb_console] FB dimensions: %dx%d\n", (uint32_t)fb->width, (uint32_t)fb->height);
    serial_printf("[fb_console] FB pitch: %d, bpp: %d\n", (uint32_t)fb->pitch, fb->bpp);
    
    recalc_console_dims();
    
    // Initialize character grid for scrollback
    chargrid_init();
    
    // Initialize with clean black screen
    fb_console_clear();
    console_x = 0;
    console_y = 0;
    
    // Reset ANSI state
    ansi_state = ANSI_STATE_NORMAL;
    ansi_dec_private = false;
    text_attrs = 0;
    default_fg = fg_color;
    default_bg = bg_color;
    cursor_enabled = true;
    scroll_top = 0;
    scroll_bottom = console_height - 1;
    alt_screen_active = false;
    
    serial_printf("[fb_console] Console chars: %dx%d\n", console_width, console_height);

    fb_size_bytes = fb->pitch * fb->height;
    real_fb_addr  = fb->address;
    /* term_init() runs from kmain() *before* kmem_init(), so kalloc() cannot
     * succeed yet.  Draw straight into the Limine framebuffer for now and let
     * fb_console_alloc_backbuffer() -- called from kmain() once the physical
     * memory manager is up -- switch the console to double buffering. */
    backbuffer    = (uint8_t *)real_fb_addr;

    serial_puts("[fb_console] Initialized framebuffer console.\n");
}

/*
    Allocate the console backbuffer and seed it with the current screen.

    Must run after kmem_init(); idempotent, so it is safe to call again.
    Returns true when double buffering is active.
*/
bool fb_console_alloc_backbuffer(void) {
    if (!fb || !real_fb_addr) {
        return false;
    }
    if (backbuffer && (void *)backbuffer != real_fb_addr) {
        return true;  /* already double buffered */
    }

    fb_size_bytes = fb->pitch * fb->height;
    uint8_t *bb = (uint8_t *)kalloc(fb_size_bytes);
    if (!bb) {
        serial_printf("[fb_console] backbuffer alloc failed (%u bytes), rendering direct\n",
                      fb_size_bytes);
        backbuffer = (uint8_t *)real_fb_addr;
        return false;
    }

    memcpy(bb, real_fb_addr, fb_size_bytes);
    backbuffer = bb;
    serial_printf("[fb_console] Backbuffer allocated: %u bytes (%ux%u, double buffered)\n",
                  fb_size_bytes, (uint32_t)fb->width, (uint32_t)fb->height);
    return true;
}

void fb_console_clear(void) {
    if (!fb) return;

    if (vmware_svga_is_available()) {
        vmware_svga_clear_screen(bg_color);
        if (backbuffer && (void*)backbuffer != real_fb_addr)
            memset(backbuffer, 0, fb_size_bytes);
        console_x = 0;
        console_y = 0;
        return;
    }

    if (amd_gpu_is_available()) {
        amd_gpu_clear_screen(bg_color);
        if (backbuffer && (void*)backbuffer != real_fb_addr)
            memset(backbuffer, 0, fb_size_bytes);
        console_x = 0;
        console_y = 0;
        return;
    }

    fillrect(bg_color, 0, 0, fb->width, fb->height);
    console_x = 0;
    console_y = 0;
    fb_flush();
}

static bool cursor_visible = false;
static uint32_t blink_counter = 0;
#define CURSOR_BLINK_INTERVAL 60

static void fb_draw_cursor(bool show){
    if(!fb) return;
    uint32_t start_x = console_x * char_width;
    uint32_t start_y = console_y * char_height;
    uint32_t cursor_color = fg_color;
    if(show){
        // inverse video: swap fg/bg for block bottom line
        fillrect(cursor_color, start_x, start_y + char_height-1, char_width, 1);
    }else{
        fillrect(bg_color, start_x, start_y + char_height-1, char_width, 1);
    }
}

void fb_cursor_blink_tick(){
    extern bool desktop_is_running(void);
    if (desktop_is_running()) return;
    if(++blink_counter < CURSOR_BLINK_INTERVAL) return;
    blink_counter = 0;
    cursor_visible = !cursor_visible;
    fb_draw_cursor(cursor_visible);
    fb_flush();
}

void fb_console_putchar(char c) {
    if (!fb) return;

    /* ---- ANSI escape sequence state machine ---- */
    if (ansi_state == ANSI_STATE_ESC) {
        if (c == '[') {
            ansi_state = ANSI_STATE_CSI;
            ansi_param_count = 0;
            ansi_cur_param = 0;
            ansi_param_started = false;
            ansi_dec_private = false;
            return;
        }
        if (c == ']') {
            /* OSC – Operating System Command: absorb until ST (BEL or ESC \) */
            ansi_state = ANSI_STATE_OSC;
            return;
        }
        if (c == '7') {
            /* ESC 7 – Save Cursor (DECSC) */
            saved_cursor_x = console_x;
            saved_cursor_y = console_y;
            saved_fg = fg_color;
            saved_bg = bg_color;
            saved_attrs = text_attrs;
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
        if (c == '8') {
            /* ESC 8 – Restore Cursor (DECRC) */
            console_x = saved_cursor_x;
            console_y = saved_cursor_y;
            fg_color = saved_fg;
            bg_color = saved_bg;
            text_attrs = saved_attrs;
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
        if (c == 'D') {
            /* ESC D – Index (scroll up if at bottom of scroll region) */
            uint32_t bot = scroll_bottom ? scroll_bottom : (console_height - 1);
            if (console_y >= bot) {
                fb_scroll_region_up(scroll_top, bot);
            } else {
                console_y++;
            }
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
        if (c == 'M') {
            /* ESC M – Reverse Index (scroll down if at top of scroll region) */
            if (console_y <= scroll_top) {
                uint32_t bot = scroll_bottom ? scroll_bottom : (console_height - 1);
                fb_scroll_region_down(scroll_top, bot);
            } else {
                console_y--;
            }
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
        if (c == 'c') {
            /* ESC c – Full Reset (RIS) */
            fg_color = default_fg;
            bg_color = default_bg;
            text_attrs = 0;
            cursor_enabled = true;
            scroll_top = 0;
            scroll_bottom = console_height - 1;
            fb_console_clear();
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
        /* Not a recognized ESC sequence – ignore ESC and re-process char */
        ansi_state = ANSI_STATE_NORMAL;
        /* Fall through to normal processing */
    }

    if (ansi_state == ANSI_STATE_OSC) {
        /* Absorb OSC until BEL (0x07) or ST (ESC \) */
        if (c == 0x07 || c == '\\') {
            ansi_state = ANSI_STATE_NORMAL;
        }
        /* Could also check for ESC arriving (to handle ESC \ as ST) */
        return;
    }

    if (ansi_state == ANSI_STATE_CSI) {
        /* Check for DEC private mode marker '?' right after ESC[ */
        if (c == '?' && !ansi_param_started && ansi_param_count == 0) {
            ansi_dec_private = true;
            return;
        }
        if (c >= '0' && c <= '9') {
            ansi_cur_param = ansi_cur_param * 10 + (c - '0');
            ansi_param_started = true;
            return;
        }
        if (c == ';') {
            if (ansi_param_count < ANSI_MAX_PARAMS) {
                ansi_params[ansi_param_count++] = ansi_param_started ? ansi_cur_param : 0;
            }
            ansi_cur_param = 0;
            ansi_param_started = false;
            return;
        }
        /* Final character of CSI sequence (letter) */
        if (c >= 0x40 && c <= 0x7E) {
            /* Push last parameter */
            if (ansi_param_started && ansi_param_count < ANSI_MAX_PARAMS) {
                ansi_params[ansi_param_count++] = ansi_cur_param;
            }
            if (ansi_dec_private) {
                ansi_handle_dec_mode(c);
            } else {
                ansi_handle_csi(c);
            }
            ansi_state = ANSI_STATE_NORMAL;
            ansi_dec_private = false;
            return;
        }
        /* Unknown byte in CSI – abort sequence */
        ansi_state = ANSI_STATE_NORMAL;
        ansi_dec_private = false;
        return;
    }

    /* ---- Normal character processing ---- */
    if ((unsigned char)c == 0x1B) { /* ESC */
        ansi_state = ANSI_STATE_ESC;
        return;
    }

    // When user types, snap scrollback to live view
    if (scrollback_offset != 0) {
        scrollback_offset = 0;
        fb_console_redraw_with_scrollback();
    }

    // remove old cursor before modifying text
    if(cursor_visible){ fb_draw_cursor(false); cursor_visible=false; }

    if (c == '\n') {
        console_x = 0;
        console_y++;
    } else if (c == '\r') {
        console_x = 0;
    } else if (c == '\t') {
        /* Tab: advance to next tab stop (every TAB_STOP_SIZE columns) */
        uint32_t next_tab = (console_x / TAB_STOP_SIZE + 1) * TAB_STOP_SIZE;
        if (next_tab >= console_width) next_tab = console_width - 1;
        /* Fill tab space with blanks in chargrid */
        while (console_x < next_tab) {
            if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS && console_x < CHARGRID_MAX_COLS) {
                chargrid[console_y][console_x] = ' ';
                chargrid_fg[console_y][console_x] = fg_color;
                chargrid_bg_arr[console_y][console_x] = bg_color;
            }
            fb_draw_char_at(console_x * char_width, console_y * char_height, ' ', fg_color, bg_color);
            console_x++;
        }
    } else if (c == '\b') {
        if (console_x > 0) {
            console_x--;
            // Clear the character area
            uint32_t start_x = console_x * char_width;
            uint32_t start_y = console_y * char_height;
            fillrect(bg_color, start_x, start_y, char_width, char_height);
            /* Update chargrid */
            if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS && console_x < CHARGRID_MAX_COLS) {
                chargrid[console_y][console_x] = ' ';
                chargrid_fg[console_y][console_x] = fg_color;
                chargrid_bg_arr[console_y][console_x] = bg_color;
            }
        }
    } else if (c >= 32 && c <= 126) {  // Printable ASCII characters
        /* Update chargrid */
        if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS && console_x < CHARGRID_MAX_COLS) {
            chargrid[console_y][console_x] = c;
            chargrid_fg[console_y][console_x] = fg_color;
            chargrid_bg_arr[console_y][console_x] = bg_color;
        }

        /* While a batched repaint is queued the pixels are rebuilt from the
         * chargrid, so drawing the glyph now would be thrown away. */
        if (!console_repaint_pending)
            fb_draw_char_at(console_x * char_width, console_y * char_height, c, fg_color, bg_color);
        console_x++;
    } else if (c > 0) {  // Handle other characters with a simple block
        uint32_t start_x = console_x * char_width;
        uint32_t start_y = console_y * char_height;
        
        // Draw a simple hollow rectangle for unknown characters
        for (uint32_t y = 0; y < char_height; y++) {
            for (uint32_t x = 0; x < char_width; x++) {
                uint32_t pixel_x = start_x + x;
                uint32_t pixel_y = start_y + y;
                
                if (pixel_x >= fb->width || pixel_y >= fb->height) {
                    continue;
                }
                
                // Draw border for unknown characters
                if (x == 0 || x == char_width-1 || y == 0 || y == char_height-1) {
                    putpixel(pixel_x, pixel_y, fg_color);
                } else {
                    putpixel(pixel_x, pixel_y, bg_color);
                }
            }
        }
        console_x++;
    }

    // Handle line wrapping
    if (console_x >= console_width) {
        console_x = 0;
        console_y++;
    }

    // Handle scrolling using scroll region
    {
        uint32_t bot = scroll_bottom ? scroll_bottom : (console_height - 1);
        if (console_y > bot) {
            fb_scroll_region_up(scroll_top, bot);
            console_y = bot;
        } else if (console_y >= console_height) {
            fb_console_scroll();
            console_y = console_height - 1;
        }
    }
    // draw cursor after processing (if enabled)
    if (cursor_enabled) {
        fb_draw_cursor(true);
        cursor_visible = true;
    }
}

void fb_console_scroll(void) {
    if (!fb) return;

    scrollback_store_screen_line(0);
    chargrid_scroll_up();

    uint8_t *target = backbuffer ? backbuffer : (uint8_t*)fb->address;
    uint32_t row_bytes = fb->pitch;
    uint32_t scroll_bytes = char_height * row_bytes;
    uint32_t total_bytes  = fb->height * row_bytes;

    uint32_t last_line_y = (console_height - 1) * char_height;

    /* Same batching rule as fb_scroll_region_up(). */
    if (console_scrolls_since_paint == 0) {
        memmove(target, target + scroll_bytes, total_bytes - scroll_bytes);
        fillrect(bg_color, 0, last_line_y, fb->width, char_height);
        fb_mark_dirty(0, fb->height);
    } else {
        console_repaint_pending = true;
        fb_mark_dirty(0, fb->height);
    }
    console_scrolls_since_paint++;
}

/* ================================================================
 * ANSI escape sequence handler – processes the final CSI command
 * ================================================================ */
static void ansi_handle_sgr(void) {
    /* SGR – Select Graphic Rendition (ESC [ ... m) */
    if (ansi_param_count == 0) {
        /* ESC[m  =  ESC[0m  = reset */
        fg_color = default_fg;
        bg_color = default_bg;
        text_attrs = 0;
        return;
    }
    for (int i = 0; i < ansi_param_count; i++) {
        int p = ansi_params[i];
        if (p == 0) {
            /* Reset all attributes */
            fg_color = default_fg;
            bg_color = default_bg;
            text_attrs = 0;
        } else if (p == 1) {
            /* Bold – brighten foreground */
            text_attrs |= ATTR_BOLD;
            fg_color |= 0x808080;
        } else if (p == 2) {
            /* Dim – reduce intensity */
            text_attrs |= ATTR_DIM;
            fg_color = ((fg_color >> 1) & 0x7F7F7F);
        } else if (p == 3) {
            /* Italic */
            text_attrs |= ATTR_ITALIC;
        } else if (p == 4) {
            /* Underline */
            text_attrs |= ATTR_UNDERLINE;
        } else if (p == 5 || p == 6) {
            /* Blink (slow/rapid) – we treat the same */
            text_attrs |= ATTR_BLINK;
        } else if (p == 7) {
            /* Inverse/reverse video */
            text_attrs |= ATTR_INVERSE;
        } else if (p == 8) {
            /* Hidden/conceal */
            text_attrs |= ATTR_HIDDEN;
        } else if (p == 9) {
            /* Strikethrough */
            text_attrs |= ATTR_STRIKETHROUGH;
        } else if (p == 22) {
            /* Normal intensity (neither bold nor dim) */
            text_attrs &= ~(ATTR_BOLD | ATTR_DIM);
        } else if (p == 23) {
            /* Not italic */
            text_attrs &= ~ATTR_ITALIC;
        } else if (p == 24) {
            /* Not underlined */
            text_attrs &= ~ATTR_UNDERLINE;
        } else if (p == 25) {
            /* Not blinking */
            text_attrs &= ~ATTR_BLINK;
        } else if (p == 27) {
            /* Not inverse */
            text_attrs &= ~ATTR_INVERSE;
        } else if (p == 28) {
            /* Not hidden */
            text_attrs &= ~ATTR_HIDDEN;
        } else if (p == 29) {
            /* Not strikethrough */
            text_attrs &= ~ATTR_STRIKETHROUGH;
        } else if (p >= 30 && p <= 37) {
            fg_color = ansi_color_table[p - 30];
            if (text_attrs & ATTR_BOLD) fg_color |= 0x808080;
        } else if (p == 38) {
            /* Extended foreground color: 38;5;N (256-color) or 38;2;R;G;B (truecolor) */
            if (i + 1 < ansi_param_count && ansi_params[i + 1] == 5) {
                /* 256-color: ESC[38;5;Nm */
                if (i + 2 < ansi_param_count) {
                    fg_color = ansi_256_to_rgb(ansi_params[i + 2]);
                    i += 2;
                }
            } else if (i + 1 < ansi_param_count && ansi_params[i + 1] == 2) {
                /* Truecolor: ESC[38;2;R;G;Bm */
                if (i + 4 < ansi_param_count) {
                    uint8_t r = (uint8_t)(ansi_params[i + 2] & 0xFF);
                    uint8_t g = (uint8_t)(ansi_params[i + 3] & 0xFF);
                    uint8_t b = (uint8_t)(ansi_params[i + 4] & 0xFF);
                    fg_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    i += 4;
                }
            }
        } else if (p == 39) {
            fg_color = default_fg; /* default fg */
        } else if (p >= 40 && p <= 47) {
            bg_color = ansi_color_table[p - 40];
        } else if (p == 48) {
            /* Extended background color: 48;5;N (256-color) or 48;2;R;G;B (truecolor) */
            if (i + 1 < ansi_param_count && ansi_params[i + 1] == 5) {
                /* 256-color: ESC[48;5;Nm */
                if (i + 2 < ansi_param_count) {
                    bg_color = ansi_256_to_rgb(ansi_params[i + 2]);
                    i += 2;
                }
            } else if (i + 1 < ansi_param_count && ansi_params[i + 1] == 2) {
                /* Truecolor: ESC[48;2;R;G;Bm */
                if (i + 4 < ansi_param_count) {
                    uint8_t r = (uint8_t)(ansi_params[i + 2] & 0xFF);
                    uint8_t g = (uint8_t)(ansi_params[i + 3] & 0xFF);
                    uint8_t b = (uint8_t)(ansi_params[i + 4] & 0xFF);
                    bg_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    i += 4;
                }
            }
        } else if (p == 49) {
            bg_color = default_bg; /* default bg */
        } else if (p >= 90 && p <= 97) {
            fg_color = ansi_color_table[p - 90 + 8]; /* bright fg */
        } else if (p >= 100 && p <= 107) {
            bg_color = ansi_color_table[p - 100 + 8]; /* bright bg */
        }
    }
}

static void ansi_handle_csi(char final_char) {
    int n = (ansi_param_count > 0) ? ansi_params[0] : 1;
    int m_param = (ansi_param_count > 1) ? ansi_params[1] : 1;

    switch (final_char) {
        case 'm': /* SGR */
            ansi_handle_sgr();
            break;
        case 'A': /* Cursor Up */
            if (n < 1) n = 1;
            if ((int)console_y >= n) console_y -= n;
            else console_y = 0;
            break;
        case 'B': /* Cursor Down */
            if (n < 1) n = 1;
            console_y += n;
            if (console_y >= console_height) console_y = console_height - 1;
            break;
        case 'C': /* Cursor Forward */
            if (n < 1) n = 1;
            console_x += n;
            if (console_x >= console_width) console_x = console_width - 1;
            break;
        case 'D': /* Cursor Back */
            if (n < 1) n = 1;
            if ((int)console_x >= n) console_x -= n;
            else console_x = 0;
            break;
        case 'E': /* Cursor Next Line (CNL) */
            if (n < 1) n = 1;
            console_x = 0;
            console_y += n;
            if (console_y >= console_height) console_y = console_height - 1;
            break;
        case 'F': /* Cursor Previous Line (CPL) */
            if (n < 1) n = 1;
            console_x = 0;
            if ((int)console_y >= n) console_y -= n;
            else console_y = 0;
            break;
        case 'G': /* Cursor Horizontal Absolute (CHA) */
            if (n < 1) n = 1;
            console_x = (uint32_t)(n - 1);
            if (console_x >= console_width) console_x = console_width - 1;
            break;
        case 'H': /* Cursor Position  ESC[row;colH */
        case 'f': /* Cursor Position (alternate) */
            if (ansi_param_count == 0) { n = 1; m_param = 1; }
            if (n < 1) n = 1;
            if (m_param < 1) m_param = 1;
            console_y = (uint32_t)(n - 1);
            console_x = (uint32_t)(m_param - 1);
            if (console_y >= console_height) console_y = console_height - 1;
            if (console_x >= console_width) console_x = console_width - 1;
            break;
        case 'J': /* Erase in Display */
            n = (ansi_param_count > 0) ? ansi_params[0] : 0;
            if (n == 2 || n == 3) {
                /* Clear entire screen (3 = also clear scrollback) */
                fb_console_clear();
                console_x = 0;
                console_y = 0;
                /* Clear chargrid */
                if (chargrid_initialized) {
                    for (uint32_t r = 0; r < console_height && r < CHARGRID_MAX_ROWS; r++) {
                        for (uint32_t c2 = 0; c2 < console_width && c2 < CHARGRID_MAX_COLS; c2++) {
                            chargrid[r][c2] = ' ';
                            chargrid_fg[r][c2] = fg_color;
                            chargrid_bg_arr[r][c2] = bg_color;
                        }
                    }
                }
            } else if (n == 0) {
                /* Clear from cursor to end of screen */
                uint32_t start_x = console_x * char_width;
                uint32_t start_y = console_y * char_height;
                /* Clear rest of current line */
                fillrect(bg_color, start_x, start_y, fb->width - start_x, char_height);
                /* Clear chargrid for rest of current line */
                if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS) {
                    for (uint32_t c2 = console_x; c2 < console_width && c2 < CHARGRID_MAX_COLS; c2++) {
                        chargrid[console_y][c2] = ' ';
                        chargrid_fg[console_y][c2] = fg_color;
                        chargrid_bg_arr[console_y][c2] = bg_color;
                    }
                }
                /* Clear remaining lines */
                if (console_y + 1 < console_height) {
                    fillrect(bg_color, 0, (console_y + 1) * char_height,
                             fb->width, (console_height - console_y - 1) * char_height);
                    if (chargrid_initialized) {
                        for (uint32_t r = console_y + 1; r < console_height && r < CHARGRID_MAX_ROWS; r++) {
                            for (uint32_t c2 = 0; c2 < console_width && c2 < CHARGRID_MAX_COLS; c2++) {
                                chargrid[r][c2] = ' ';
                                chargrid_fg[r][c2] = fg_color;
                                chargrid_bg_arr[r][c2] = bg_color;
                            }
                        }
                    }
                }
            } else if (n == 1) {
                /* Clear from start of screen to cursor */
                if (console_y > 0) {
                    fillrect(bg_color, 0, 0, fb->width, console_y * char_height);
                }
                fillrect(bg_color, 0, console_y * char_height, (console_x + 1) * char_width, char_height);
            }
            break;
        case 'K': /* Erase in Line */
            n = (ansi_param_count > 0) ? ansi_params[0] : 0;
            if (n == 0) {
                /* Clear from cursor to end of line */
                uint32_t x0 = console_x * char_width;
                uint32_t y0 = console_y * char_height;
                fillrect(bg_color, x0, y0, fb->width - x0, char_height);
                if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS) {
                    for (uint32_t c2 = console_x; c2 < console_width && c2 < CHARGRID_MAX_COLS; c2++) {
                        chargrid[console_y][c2] = ' ';
                        chargrid_fg[console_y][c2] = fg_color;
                        chargrid_bg_arr[console_y][c2] = bg_color;
                    }
                }
            } else if (n == 1) {
                /* Clear from start of line to cursor */
                uint32_t y0 = console_y * char_height;
                fillrect(bg_color, 0, y0, (console_x + 1) * char_width, char_height);
                if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS) {
                    for (uint32_t c2 = 0; c2 <= console_x && c2 < CHARGRID_MAX_COLS; c2++) {
                        chargrid[console_y][c2] = ' ';
                        chargrid_fg[console_y][c2] = fg_color;
                        chargrid_bg_arr[console_y][c2] = bg_color;
                    }
                }
            } else if (n == 2) {
                /* Clear entire line */
                uint32_t y0 = console_y * char_height;
                fillrect(bg_color, 0, y0, fb->width, char_height);
                if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS) {
                    for (uint32_t c2 = 0; c2 < console_width && c2 < CHARGRID_MAX_COLS; c2++) {
                        chargrid[console_y][c2] = ' ';
                        chargrid_fg[console_y][c2] = fg_color;
                        chargrid_bg_arr[console_y][c2] = bg_color;
                    }
                }
            }
            break;
        case 'L': { /* Insert Line(s) – shift lines down within scroll region */
            if (n < 1) n = 1;
            uint32_t bot = scroll_bottom ? scroll_bottom : (console_height - 1);
            for (int k = 0; k < n; k++) {
                fb_scroll_region_down(console_y, bot);
            }
            console_x = 0;
            break;
        }
        case 'M': { /* Delete Line(s) – shift lines up within scroll region */
            if (n < 1) n = 1;
            uint32_t bot = scroll_bottom ? scroll_bottom : (console_height - 1);
            for (int k = 0; k < n; k++) {
                fb_scroll_region_up(console_y, bot);
            }
            console_x = 0;
            break;
        }
        case 'X': { /* Erase Character(s) – erase N chars from cursor without moving */
            if (n < 1) n = 1;
            for (int k = 0; k < n && (console_x + (uint32_t)k) < console_width; k++) {
                uint32_t cx = console_x + (uint32_t)k;
                fb_draw_char_at(cx * char_width, console_y * char_height, ' ', fg_color, bg_color);
                if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS && cx < CHARGRID_MAX_COLS) {
                    chargrid[console_y][cx] = ' ';
                    chargrid_fg[console_y][cx] = fg_color;
                    chargrid_bg_arr[console_y][cx] = bg_color;
                }
            }
            break;
        }
        case 'P': { /* Delete Character(s) – shift chars left */
            if (n < 1) n = 1;
            if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS) {
                uint32_t cols = console_width;
                if (cols > CHARGRID_MAX_COLS) cols = CHARGRID_MAX_COLS;
                for (uint32_t c2 = console_x; c2 + (uint32_t)n < cols; c2++) {
                    chargrid[console_y][c2] = chargrid[console_y][c2 + n];
                    chargrid_fg[console_y][c2] = chargrid_fg[console_y][c2 + n];
                    chargrid_bg_arr[console_y][c2] = chargrid_bg_arr[console_y][c2 + n];
                }
                for (uint32_t c2 = (cols > (uint32_t)n) ? cols - (uint32_t)n : 0; c2 < cols; c2++) {
                    chargrid[console_y][c2] = ' ';
                    chargrid_fg[console_y][c2] = fg_color;
                    chargrid_bg_arr[console_y][c2] = bg_color;
                }
                fb_redraw_chargrid_line(console_y);
            }
            break;
        }
        case '@': { /* Insert Character(s) – shift chars right */
            if (n < 1) n = 1;
            if (chargrid_initialized && console_y < CHARGRID_MAX_ROWS) {
                uint32_t cols = console_width;
                if (cols > CHARGRID_MAX_COLS) cols = CHARGRID_MAX_COLS;
                /* Shift right from end */
                for (int c2 = (int)cols - 1; c2 >= (int)console_x + n; c2--) {
                    chargrid[console_y][c2] = chargrid[console_y][c2 - n];
                    chargrid_fg[console_y][c2] = chargrid_fg[console_y][c2 - n];
                    chargrid_bg_arr[console_y][c2] = chargrid_bg_arr[console_y][c2 - n];
                }
                /* Clear inserted positions */
                for (int c2 = (int)console_x; c2 < (int)console_x + n && c2 < (int)cols; c2++) {
                    chargrid[console_y][c2] = ' ';
                    chargrid_fg[console_y][c2] = fg_color;
                    chargrid_bg_arr[console_y][c2] = bg_color;
                }
                fb_redraw_chargrid_line(console_y);
            }
            break;
        }
        case 's': /* Save Cursor Position (ANSI.SYS) */
            saved_cursor_x = console_x;
            saved_cursor_y = console_y;
            saved_fg = fg_color;
            saved_bg = bg_color;
            saved_attrs = text_attrs;
            break;
        case 'u': /* Restore Cursor Position (ANSI.SYS) */
            console_x = saved_cursor_x;
            console_y = saved_cursor_y;
            fg_color = saved_fg;
            bg_color = saved_bg;
            text_attrs = saved_attrs;
            break;
        case 'r': { /* Set Scroll Region (DECSTBM) ESC[top;bottomr */
            uint32_t top = (ansi_param_count > 0 && ansi_params[0] > 0) ? (uint32_t)(ansi_params[0] - 1) : 0;
            uint32_t bot = (ansi_param_count > 1 && ansi_params[1] > 0) ? (uint32_t)(ansi_params[1] - 1) : (console_height - 1);
            if (top >= console_height) top = 0;
            if (bot >= console_height) bot = console_height - 1;
            if (top < bot) {
                scroll_top = top;
                scroll_bottom = bot;
            } else {
                scroll_top = 0;
                scroll_bottom = console_height - 1;
            }
            /* Move cursor to home position */
            console_x = 0;
            console_y = scroll_top;
            break;
        }
        case 'n': { /* Device Status Report (DSR) */
            n = (ansi_param_count > 0) ? ansi_params[0] : 0;
            if (n == 6) {
                /* Report Cursor Position: ESC[row;colR */
                char cpr[32];
                int len = snprintf(cpr, sizeof(cpr), "\x1b[%d;%dR",
                                   (int)(console_y + 1), (int)(console_x + 1));
                if (len > 0) {
                    /* Feed response back into keyboard buffer */
                    extern void keyboard_inject_string(const char *s, int len);
                    keyboard_inject_string(cpr, len);
                }
            } else if (n == 5) {
                /* Device OK: ESC[0n */
                extern void keyboard_inject_string(const char *s, int len);
                keyboard_inject_string("\x1b[0n", 4);
            }
            break;
        }
        case 'd': { /* Vertical Position Absolute (VPA) */
            if (n < 1) n = 1;
            console_y = (uint32_t)(n - 1);
            if (console_y >= console_height) console_y = console_height - 1;
            break;
        }
        case 'S': { /* Scroll Up – scroll content up N lines */
            if (n < 1) n = 1;
            uint32_t bot = scroll_bottom ? scroll_bottom : (console_height - 1);
            for (int k = 0; k < n; k++) {
                fb_scroll_region_up(scroll_top, bot);
            }
            break;
        }
        case 'T': { /* Scroll Down – scroll content down N lines */
            if (n < 1) n = 1;
            uint32_t bot = scroll_bottom ? scroll_bottom : (console_height - 1);
            for (int k = 0; k < n; k++) {
                fb_scroll_region_down(scroll_top, bot);
            }
            break;
        }
        default:
            /* Unknown CSI sequence – ignore */
            break;
    }
}

/* ================================================================
 * DEC private mode handler (ESC [ ? ... h/l)
 * ================================================================ */
static void ansi_handle_dec_mode(char final_char) {
    int mode = (ansi_param_count > 0) ? ansi_params[0] : 0;
    bool set = (final_char == 'h');  /* h = set, l = reset */

    switch (mode) {
        case 25: /* Cursor visibility (DECTCEM) */
            cursor_enabled = set;
            if (!set) {
                /* Hide cursor immediately */
                fb_draw_cursor(false);
                cursor_visible = false;
            }
            break;
        case 7: /* Auto-wrap mode (DECAWM) – we always auto-wrap, ignore */
            break;
        case 1049: /* Alternate screen buffer (xterm) */
            if (set && !alt_screen_active) {
                /* Save main screen, switch to alt */
                alt_cursor_x = console_x;
                alt_cursor_y = console_y;
                /* Save main chargrid to alt buffers */
                for (uint32_t r = 0; r < console_height && r < ALT_CHARGRID_MAX_ROWS; r++) {
                    for (uint32_t c2 = 0; c2 < console_width && c2 < ALT_CHARGRID_MAX_COLS; c2++) {
                        alt_chargrid[r][c2] = chargrid[r][c2];
                        alt_chargrid_fg[r][c2] = chargrid_fg[r][c2];
                        alt_chargrid_bg[r][c2] = chargrid_bg_arr[r][c2];
                    }
                }
                alt_screen_active = true;
                /* Clear screen for alternate buffer */
                fb_console_clear();
                if (chargrid_initialized) {
                    for (uint32_t r = 0; r < CHARGRID_MAX_ROWS; r++)
                        for (uint32_t c2 = 0; c2 < CHARGRID_MAX_COLS; c2++) {
                            chargrid[r][c2] = ' ';
                            chargrid_fg[r][c2] = default_fg;
                            chargrid_bg_arr[r][c2] = default_bg;
                        }
                }
                console_x = 0;
                console_y = 0;
            } else if (!set && alt_screen_active) {
                /* Restore main screen from alt */
                for (uint32_t r = 0; r < console_height && r < ALT_CHARGRID_MAX_ROWS; r++) {
                    for (uint32_t c2 = 0; c2 < console_width && c2 < ALT_CHARGRID_MAX_COLS; c2++) {
                        chargrid[r][c2] = alt_chargrid[r][c2];
                        chargrid_fg[r][c2] = alt_chargrid_fg[r][c2];
                        chargrid_bg_arr[r][c2] = alt_chargrid_bg[r][c2];
                    }
                }
                console_x = alt_cursor_x;
                console_y = alt_cursor_y;
                alt_screen_active = false;
                /* Redraw main screen */
                for (uint32_t r = 0; r < console_height; r++) {
                    fb_redraw_chargrid_line(r);
                }
            }
            break;
        case 47: /* Alternate screen (simpler, same as 1049 for us) */
        case 1047:
            /* Treat like 1049 */
            ansi_params[0] = 1049;
            ansi_handle_dec_mode(final_char);
            break;
        default:
            /* Unknown DEC private mode – ignore */
            break;
    }
}

/* ================================================================
 * Scroll region helpers
 * ================================================================ */

/* Scroll content up by one line within [top..bottom] row range (0-based) */
static void fb_scroll_region_up(uint32_t top, uint32_t bottom) {
    if (!fb || top >= bottom) return;
    if (top >= console_height) return;
    if (bottom >= console_height) bottom = console_height - 1;

    /* If scrolling the whole screen, store top line to scrollback */
    if (top == 0 && bottom == console_height - 1) {
        scrollback_store_screen_line(0);
    }

    /* Shift chargrid rows up within region */
    if (chargrid_initialized) {
        uint32_t cols = console_width;
        if (cols > CHARGRID_MAX_COLS) cols = CHARGRID_MAX_COLS;
        for (uint32_t r = top + 1; r <= bottom && r < CHARGRID_MAX_ROWS; r++) {
            for (uint32_t c2 = 0; c2 < cols; c2++) {
                chargrid[r - 1][c2] = chargrid[r][c2];
                chargrid_fg[r - 1][c2] = chargrid_fg[r][c2];
                chargrid_bg_arr[r - 1][c2] = chargrid_bg_arr[r][c2];
            }
        }
        /* Clear bottom row of region */
        if (bottom < CHARGRID_MAX_ROWS) {
            for (uint32_t c2 = 0; c2 < cols; c2++) {
                chargrid[bottom][c2] = ' ';
                chargrid_fg[bottom][c2] = default_fg;
                chargrid_bg_arr[bottom][c2] = default_bg;
            }
        }
    }

    uint8_t *target = backbuffer ? backbuffer : (uint8_t*)fb->address;
    uint32_t y_src = (top + 1) * char_height;
    uint32_t y_dst = top * char_height;
    uint32_t y_end = (bottom + 1) * char_height;
    if (y_end > fb->height) y_end = fb->height;

    /* Batched scrolling: the first line of a burst shifts the pixels directly,
     * further lines before the next repaint only update the chargrid and set
     * the repaint flag (see console_scrolls_since_paint). */
    if (console_scrolls_since_paint == 0) {
        uint32_t copy_bytes = (y_end - y_src) * fb->pitch;
        memmove(target + y_dst * fb->pitch, target + y_src * fb->pitch, copy_bytes);

        fillrect(bg_color, 0, bottom * char_height, fb->width, char_height);
        fb_mark_dirty(y_dst, y_end - y_dst + char_height);
    } else {
        console_repaint_pending = true;
        fb_mark_dirty(y_dst, y_end - y_dst + char_height);
    }
    console_scrolls_since_paint++;
}

/* Scroll content down by one line within [top..bottom] row range (0-based) */
static void fb_scroll_region_down(uint32_t top, uint32_t bottom) {
    if (!fb || top >= bottom) return;
    if (top >= console_height) return;
    if (bottom >= console_height) bottom = console_height - 1;

    /* Shift chargrid rows down within region */
    if (chargrid_initialized) {
        uint32_t cols = console_width;
        if (cols > CHARGRID_MAX_COLS) cols = CHARGRID_MAX_COLS;
        for (uint32_t r = bottom; r > top && r < CHARGRID_MAX_ROWS; r--) {
            for (uint32_t c2 = 0; c2 < cols; c2++) {
                chargrid[r][c2] = chargrid[r - 1][c2];
                chargrid_fg[r][c2] = chargrid_fg[r - 1][c2];
                chargrid_bg_arr[r][c2] = chargrid_bg_arr[r - 1][c2];
            }
        }
        /* Clear top row of region */
        if (top < CHARGRID_MAX_ROWS) {
            for (uint32_t c2 = 0; c2 < cols; c2++) {
                chargrid[top][c2] = ' ';
                chargrid_fg[top][c2] = default_fg;
                chargrid_bg_arr[top][c2] = default_bg;
            }
        }
    }

    uint8_t *target = backbuffer ? backbuffer : (uint8_t*)fb->address;
    uint32_t y_src = top * char_height;
    uint32_t y_dst = y_src + char_height;
    uint32_t y_end = (bottom + 1) * char_height;
    if (y_end > fb->height) y_end = fb->height;
    if (y_dst + (y_end - y_src - char_height * 1) > fb->height) {
        y_end = fb->height;
    }
    uint32_t region_bytes = (y_end - y_src - char_height) * fb->pitch;
    if (region_bytes > 0 && y_dst * fb->pitch + region_bytes <= fb_size_bytes) {
        memmove(target + y_dst * fb->pitch, target + y_src * fb->pitch, region_bytes);
    }

    fillrect(bg_color, 0, top * char_height, fb->width, char_height);
    fb_mark_dirty(y_src, y_end - y_src);
}

/* ================================================================
 * Scrollback display functions
 * ================================================================ */

/* Draw a single character at pixel position (px, py) using the 16x8 font
 * Also applies text attributes: underline, strikethrough, inverse */
static void fb_draw_char_at(uint32_t px, uint32_t py, char c,
                            uint32_t fg, uint32_t bg_c)
{
    if (!fb) return;
    if (c < 32 || c > 126) c = ' ';

    uint32_t draw_fg = fg;
    uint32_t draw_bg = bg_c;
    if (text_attrs & ATTR_INVERSE) { draw_fg = bg_c; draw_bg = fg; }
    if (text_attrs & ATTR_HIDDEN)  { draw_fg = draw_bg; }

    const uint8_t *bmp = font_16x8[(unsigned char)c - 32];

    if (fb->bpp == 32 && font_scale == 1) {
        uint32_t *screen = (uint32_t*)(backbuffer ? backbuffer : (uint8_t*)fb->address);
        uint32_t ppl = fb->pitch / 4;

        for (uint32_t row = 0; row < 16; row++) {
            uint32_t y = py + row;
            if (y >= fb->height) break;
            uint8_t line = bmp[row];
            uint32_t *dest = screen + y * ppl + px;
            uint32_t end_x = px + 8;
            if (end_x > fb->width) end_x = fb->width;
            for (uint32_t x = px; x < end_x; x++) {
                dest[x - px] = (line & (0x80 >> (x - px))) ? draw_fg : draw_bg;
            }
        }

        if (text_attrs & ATTR_UNDERLINE) {
            uint32_t ul_y = py + 15;
            if (ul_y < fb->height) {
                uint32_t *dest = screen + ul_y * ppl + px;
                uint32_t end_x = px + 8;
                if (end_x > fb->width) end_x = fb->width;
                for (uint32_t i = 0; i < end_x - px; i++) dest[i] = draw_fg;
            }
        }
        if (text_attrs & ATTR_STRIKETHROUGH) {
            uint32_t st_y = py + 8;
            if (st_y < fb->height) {
                uint32_t *dest = screen + st_y * ppl + px;
                uint32_t end_x = px + 8;
                if (end_x > fb->width) end_x = fb->width;
                for (uint32_t i = 0; i < end_x - px; i++) dest[i] = draw_fg;
            }
        }
        fb_mark_dirty(py, 16);
        return;
    }

    for (uint32_t row = 0; row < 16; row++) {
        uint8_t line = bmp[row];
        for (uint32_t col = 0; col < 8; col++) {
            bool on = line & (0x80 >> col);
            for (uint32_t dx = 0; dx < font_scale; dx++) {
                for (uint32_t dy = 0; dy < font_scale; dy++) {
                    uint32_t x = px + col * font_scale + dx;
                    uint32_t y = py + row * font_scale + dy;
                    if (x >= fb->width || y >= fb->height) continue;
                    putpixel(x, y, on ? draw_fg : draw_bg);
                }
            }
        }
    }

    if (text_attrs & ATTR_UNDERLINE) {
        uint32_t ul_y = py + (16 - 1) * font_scale;
        for (uint32_t col = 0; col < 8 * font_scale; col++) {
            uint32_t x = px + col;
            if (x < fb->width && ul_y < fb->height)
                putpixel(x, ul_y, draw_fg);
        }
    }
    if (text_attrs & ATTR_STRIKETHROUGH) {
        uint32_t st_y = py + 8 * font_scale;
        for (uint32_t col = 0; col < 8 * font_scale; col++) {
            uint32_t x = px + col;
            if (x < fb->width && st_y < fb->height)
                putpixel(x, st_y, draw_fg);
        }
    }
}

/* Redraw screen row `scr_row` from the chargrid */
static void fb_redraw_chargrid_line(uint32_t scr_row) {
    if (!chargrid_initialized || !fb) return;
    if (scr_row >= CHARGRID_MAX_ROWS || scr_row >= console_height) return;
    uint32_t cols = console_width;
    if (cols > CHARGRID_MAX_COLS) cols = CHARGRID_MAX_COLS;
    uint32_t py = scr_row * char_height;
    for (uint32_t c = 0; c < cols; c++) {
        fb_draw_char_at(c * char_width, py, chargrid[scr_row][c],
                        chargrid_fg[scr_row][c], chargrid_bg_arr[scr_row][c]);
    }
}

/* Redraw screen row `scr_row` from scrollback index `sb_idx` */
static void fb_redraw_scrollback_line(uint32_t scr_row, uint32_t sb_idx) {
    if (!fb) return;
    uint32_t cols = console_width;
    if (cols > SCROLLBACK_COLS_MAX) cols = SCROLLBACK_COLS_MAX;
    uint32_t py = scr_row * char_height;
    for (uint32_t c = 0; c < cols; c++) {
        char ch = scrollback_buf[sb_idx][c];
        uint32_t fg = scrollback_fg_buf[sb_idx][c];
        if (fg == 0) fg = default_fg;
        fb_draw_char_at(c * char_width, py, ch, fg, default_bg);
    }
}

void fb_console_scroll_back(void) {
    if (scrollback_count == 0) return;
    if (scrollback_offset >= (int)scrollback_count) return;
    scrollback_offset++;
    fb_console_redraw_with_scrollback();
    fb_flush();
}

void fb_console_scroll_forward(void) {
    if (scrollback_offset <= 0) return;
    scrollback_offset--;
    fb_console_redraw_with_scrollback();
    fb_flush();
}

void fb_console_redraw_with_scrollback(void) {
    if (!fb) return;
    uint32_t rows = console_height;

    /* Clear entire screen */
    fillrect(default_bg, 0, 0, fb->width, fb->height);

    if (scrollback_offset == 0) {
        /* Normal live view – redraw from chargrid */
        for (uint32_t r = 0; r < rows; r++) {
            fb_redraw_chargrid_line(r);
        }
        return;
    }

    /* Mixed view: top part from scrollback, bottom from chargrid */
    uint32_t sb_lines = (uint32_t)scrollback_offset;
    if (sb_lines > rows) sb_lines = rows;
    uint32_t grid_lines = rows - sb_lines;

    /* Draw scrollback lines at top of screen */
    for (uint32_t i = 0; i < sb_lines; i++) {
        int idx = (int)scrollback_write - (int)scrollback_offset + (int)i;
        while (idx < 0) idx += SCROLLBACK_LINES;
        idx = idx % SCROLLBACK_LINES;
        fb_redraw_scrollback_line(i, (uint32_t)idx);
    }

    /* Draw chargrid lines (from the top of chargrid) below scrollback */
    for (uint32_t i = 0; i < grid_lines; i++) {
        uint32_t scr_row = sb_lines + i;
        /* Draw chargrid row i at screen row scr_row */
        if (i >= CHARGRID_MAX_ROWS || i >= console_height) break;
        uint32_t cols = console_width;
        if (cols > CHARGRID_MAX_COLS) cols = CHARGRID_MAX_COLS;
        uint32_t py = scr_row * char_height;
        for (uint32_t c = 0; c < cols; c++) {
            fb_draw_char_at(c * char_width, py, chargrid[i][c],
                            chargrid_fg[i][c], chargrid_bg_arr[i][c]);
        }
    }
}

void fb_console_write(const char *str, size_t len) {
    if (!fb) return;
    for (size_t i = 0; i < len; i++) {
        fb_console_putchar(str[i]);
    }
    fb_flush();
}

/* Public API wrappers for ANSI terminal control */
void console_set_cursor_visible(bool visible) {
    cursor_enabled = visible;
    if (!visible) {
        fb_draw_cursor(false);
        cursor_visible = false;
    }
}

bool console_get_cursor_visible(void) {
    return cursor_enabled;
}

void console_set_scroll_region(uint32_t top, uint32_t bottom) {
    if (top < bottom && bottom < console_height) {
        scroll_top = top;
        scroll_bottom = bottom;
    }
}

void console_save_cursor(void) {
    saved_cursor_x = console_x;
    saved_cursor_y = console_y;
    saved_fg = fg_color;
    saved_bg = bg_color;
    saved_attrs = text_attrs;
}

void console_restore_cursor(void) {
    console_x = saved_cursor_x;
    console_y = saved_cursor_y;
    fg_color = saved_fg;
    bg_color = saved_bg;
    text_attrs = saved_attrs;
}

/* ================================================================
 * Virtual console save / restore helpers
 * ================================================================ */

void fb_console_save_state(void *chars, void *fg_out, void *bg_out,
                           uint32_t *cx, uint32_t *cy,
                           uint32_t *fgc, uint32_t *bgc,
                           uint32_t *attrs,
                           uint32_t *dfg, uint32_t *dbg) {
    memcpy(chars,  chargrid,       sizeof(chargrid));
    memcpy(fg_out, chargrid_fg,    sizeof(chargrid_fg));
    memcpy(bg_out, chargrid_bg_arr, sizeof(chargrid_bg_arr));
    *cx    = console_x;
    *cy    = console_y;
    *fgc   = fg_color;
    *bgc   = bg_color;
    *attrs = text_attrs;
    *dfg   = default_fg;
    *dbg   = default_bg;
}

void fb_console_restore_state(const void *chars, const void *fg_in, const void *bg_in,
                              uint32_t cx, uint32_t cy,
                              uint32_t fgc, uint32_t bgc,
                              uint32_t attrs,
                              uint32_t dfg, uint32_t dbg) {
    memcpy(chargrid,        chars, sizeof(chargrid));
    memcpy(chargrid_fg,     fg_in, sizeof(chargrid_fg));
    memcpy(chargrid_bg_arr, bg_in, sizeof(chargrid_bg_arr));
    console_x  = cx;
    console_y  = cy;
    fg_color   = fgc;
    bg_color   = bgc;
    text_attrs = attrs;
    default_fg = dfg;
    default_bg = dbg;
    scrollback_offset = 0;
}

void fb_console_redraw_from_chargrid(void) {
    if (!fb || !chargrid_initialized) return;
    chargrid_repaint_all();
    fb_flush();
    fb_present_scanout();
}

/* Rebuild the whole console area from the chargrid.  Used after batched scrolls
 * (see console_scrolls_since_paint) and by the scrollback view; marks every
 * console row dirty so the next fb_flush copies them out. */
static void chargrid_repaint_all(void) {
    if (!fb || !chargrid_initialized) return;

    fillrect(default_bg, 0, 0, fb->width, fb->height);

    uint32_t rows = console_height;
    for (uint32_t r = 0; r < rows && r < CHARGRID_MAX_ROWS; r++) {
        fb_redraw_chargrid_line(r);
    }

    /* The cursor is not part of the chargrid - put it back. */
    if (cursor_enabled) {
        fb_draw_cursor(true);
        cursor_visible = true;
    }
}

void fb_draw_char_at_px(uint32_t px, uint32_t py, char c,
                        uint32_t fg_c, uint32_t bg_c) {
    fb_draw_char_at(px, py, c, fg_c, bg_c);
}

// Debug framebuffer request during initialization
void debug_framebuffer_request(void) {
    if(fb_rq.response && fb_rq.response->framebuffer_count > 0){
        fb = fb_rq.response->framebuffers[0];
        fb_boot_max_w = (uint32_t)fb->width;
        fb_boot_max_h = (uint32_t)fb->height;
        serial_printf("[utils] Framebuffer available. Address=%p\n", fb->address);
        serial_printf("[utils] Framebuffer dimensions: width=%u height=%u\n", fb->width, fb->height);
    } else {
        serial_puts("[utils] Framebuffer request failed or no framebuffers available.\n");
        serial_printf("[utils] Debug: fb_rq.response = %p\n", fb_rq.response);
        if (fb_rq.response) {
            serial_printf("[utils] Debug: framebuffer_count = %llu\n", fb_rq.response->framebuffer_count);
        } else {
            serial_puts("[utils] Debug: No response from framebuffer request.\n");
        }
        fb = NULL;
    }
}

/* Text console size in character cells (Linux ABI TIOCGWINSZ). */
void fb_console_get_size(uint32_t *cols, uint32_t *rows) {
    if (cols) *cols = console_width;
    if (rows) *rows = console_height;
}
