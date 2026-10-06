#include <stdint.h>

/* Syscall numbers - must match kernel syscall.h */
#define SYS_EXIT  0
#define SYS_FB_CLEAR    63
#define SYS_FB_FILLRECT 64
#define SYS_FB_GET_WIDTH 65
#define SYS_FB_GET_HEIGHT 66
#define SYS_GET_TERMINATE_FLAG 69

static inline uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    uint64_t result;
    register uint64_t r10_reg __asm__("r10") = arg4;
    register uint64_t r8_reg __asm__("r8") = arg5;
    __asm__ volatile (
        "syscall"
        : "=a" (result)
        : "a" (num), "D" (arg1), "S" (arg2), "d" (arg3), "r" (r10_reg), "r" (r8_reg)
        : "memory", "rcx", "r11"
    );
    return result;
}

/* Framebuffer via syscalls (safe from Ring 3) */
static uint64_t fb_get_width(void) { return syscall(SYS_FB_GET_WIDTH, 0, 0, 0, 0, 0); }
static uint64_t fb_get_height(void) { return syscall(SYS_FB_GET_HEIGHT, 0, 0, 0, 0, 0); }
static void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    syscall(SYS_FB_FILLRECT, color, x, y, w, h);
}
static void clear_screen(void) { syscall(SYS_FB_CLEAR, 0, 0, 0, 0, 0); }
static int get_terminate_flag(void) { return (int)syscall(SYS_GET_TERMINATE_FLAG, 0, 0, 0, 0, 0); }
static void sys_exit(int status) {
    syscall(SYS_EXIT, (uint64_t)status, 0, 0, 0, 0);
    for (;;) {}
}

// Simple delay function
void delay(uint32_t count) {
    for (volatile uint32_t i = 0; i < count; i++) {
        __asm__ volatile ("nop");
    }
}

// Generate RGB color
uint32_t make_color(uint8_t r, uint8_t g, uint8_t b) {
    return (r << 16) | (g << 8) | b;
}

void _start(void) {
    // Get screen dimensions
    uint32_t screen_width = (uint32_t)fb_get_width();
    uint32_t screen_height = (uint32_t)fb_get_height();

    // Logo properties
    uint32_t logo_width = 100;
    uint32_t logo_height = 60;
    int32_t x = 0;
    int32_t y = 0;
    int32_t dx = 2;  // Speed and direction
    int32_t dy = 2;
    uint32_t color = make_color(255, 0, 0);  // Start with red

    // Clear screen
    clear_screen();

    while (1) {
        if (get_terminate_flag()) break;

        // Clear previous position (draw black rectangle)
        fillrect(0x000000, x, y, logo_width, logo_height);

        // Update position
        x += dx;
        y += dy;

        // Bounce logic and color change
        if (x + logo_width >= screen_width || x <= 0) {
            dx = -dx;
            color = make_color(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF);  // Cycle colors
        }
        if (y + logo_height >= screen_height || y <= 0) {
            dy = -dy;
            color = make_color((color >> 8) & 0xFF, (color >> 16) & 0xFF, color & 0xFF);
        }

        // Draw logo at new position
        fillrect(color, x, y, logo_width, logo_height);

        // Delay for animation speed
        delay(10000000);
    }

    clear_screen();
    sys_exit(0);
}
