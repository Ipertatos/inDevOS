#pragma once

#include "types.h"

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

typedef struct position{
    uint8_t x;
    uint8_t y;
}position;

// Network byte order helpers
static inline uint16_t htons(uint16_t val) {
    return (val << 8) | (val >> 8);
}
static inline uint16_t ntohs(uint16_t val) {
    return (val << 8) | (val >> 8);
}
static inline uint32_t htonl(uint32_t val) {
    return (val << 24) | ((val & 0xFF00) << 8) | ((val >> 8) & 0xFF00) | (val >> 24);
}
static inline uint32_t ntohl(uint32_t val) {
    return (val << 24) | ((val & 0xFF00) << 8) | ((val >> 8) & 0xFF00) | (val >> 24);
}

void putpixel(uint64_t x, uint64_t y, uint32_t color);
void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h);

void log_panic(char *msg);

/** @brief Seed the stack-protector canary; call once, early in kmain(). */
void stack_guard_init(void);
void log_err(char *msg);
void log_warn(char *msg);
void log_info(char *msg);
void log_success(char *msg);

char *int2string(uint32_t n);
char *int2hex(uint64_t value);

char *remove_nl(char *str);
char *strtok(char *str, const char *delim);
bool strEql(const char *str1, const char *str2);

void dump_hex(char *stack);
void dump_str(char *stack);

void clear_screen();

// Interrupt context tracking functions
void enter_interrupt_context(void);
void exit_interrupt_context(void);
int in_interrupt_context(void);

// Framebuffer Console Functions
void fb_console_init(void);
/** Allocate the console backbuffer (double buffering).  Must be called after
 *  kmem_init(); fb_console_init() runs too early for kalloc().  Idempotent.
 *  Returns true when double buffering is active. */
bool fb_console_alloc_backbuffer(void);
void fb_console_putchar(char c);
void fb_console_write(const char *str, size_t len);
void fb_console_clear(void);
void fb_console_scroll(void);
void fb_cursor_blink_tick(void);
void fb_set_font_scale(uint32_t scale);
void fb_test_pattern(void);
void fb_flush(void);
/** Coalesce paints inside a burst of output (see fb_flush); when enabled the
 *  main loop must call fb_flush_pending() so the last update is painted. */
void fb_set_defer_flush(bool enable);
void fb_flush_pending(void);
/** Push full virtio scanout after fb_flush (separate Limine vs virtio buffers). */
void fb_present_scanout(void);
/** Mark entire framebuffer dirty so the next fb_flush copies the full backbuffer. */
void fb_mark_dirty_all(void);
void fb_mark_dirty_rows(uint32_t y, uint32_t h);
/** Ring-3 syscall drawing helpers (virtio scanout when available). */
void fb_app_clear(void);
void fb_app_fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
/** Pointer to pixels the compositor last drew (backbuffer if active, else Limine FB). */
const void *fb_get_composite_buffer(void);

// Scrollback functions
void fb_console_scroll_back(void);
void fb_console_scroll_forward(void);
void fb_console_redraw_with_scrollback(void);

// Framebuffer initialization
void debug_framebuffer_request(void);

void wait(uint64_t ms);

void console_set_fg(uint32_t color);
uint32_t console_get_fg(void);
uint32_t console_get_width(void);
uint32_t console_get_height(void);

// Framebuffer pixel dimensions for graphics programming
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
/** Boot framebuffer limits (cannot exceed without reboot / new mapping). */
uint32_t fb_max_width(void);
uint32_t fb_max_height(void);
/** Set logical resolution (Limine + virtio-gpu viewport when active). Returns 0 on success. */
int fb_set_video_resolution(uint32_t width, uint32_t height);

bool get_terminate_flag(void);
bool get_force_kill_flag(void);

// Program running state
bool get_program_running(void);
void set_program_running(bool value);
void set_terminate_flag(bool value);
void set_force_kill_flag(bool value);

// GPU acceleration helpers
const char* get_active_gpu_driver(void);
bool is_gpu_acceleration_available(void);

// Enhanced ANSI terminal control
void console_set_cursor_visible(bool visible);
bool console_get_cursor_visible(void);
void console_set_scroll_region(uint32_t top, uint32_t bottom);
void console_save_cursor(void);
void console_restore_cursor(void);

// Virtual console save/restore
void fb_console_save_state(void *chars, void *fg, void *bg,
                           uint32_t *cx, uint32_t *cy,
                           uint32_t *fgc, uint32_t *bgc,
                           uint32_t *attrs,
                           uint32_t *dfg, uint32_t *dbg);
void fb_console_restore_state(const void *chars, const void *fg, const void *bg,
                              uint32_t cx, uint32_t cy,
                              uint32_t fgc, uint32_t bgc,
                              uint32_t attrs,
                              uint32_t dfg, uint32_t dbg);
void fb_console_redraw_from_chargrid(void);
void fb_draw_char_at_px(uint32_t px, uint32_t py, char c,
                        uint32_t fg_c, uint32_t bg_c);
