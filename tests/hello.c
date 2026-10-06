#include <stdint.h>

// hello.c - Random colored boxes demo for inDevOS
// This draws random sized and colored rectangles on screen

// Global variables (these create relocations that need fixing)
const char *message = "Drawing random boxes...\r\n";
int counter = 0;

// Function pointers to kernel functions (addresses from kernel.sym)
void (*serial_putc)(char c) = (void (*)(char c))0xffffffff8000101e;
void (*fillrect)(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) = (void (*)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t))0xffffffff800050dd;
uint32_t (*fb_get_width)(void) = (uint32_t (*)(void))0xffffffff8000598b;
uint32_t (*fb_get_height)(void) = (uint32_t (*)(void))0xffffffff800059b3;

// Simple PRNG (Linear Congruential Generator)
uint32_t rng_state = 12345;

uint32_t random(void) {
    rng_state = rng_state * 1103515245 + 12345;
    return rng_state;
}

uint32_t random_range(uint32_t min, uint32_t max) {
    return min + (random() % (max - min + 1));
}

// Generate random RGB color
uint32_t random_color(void) {
    uint8_t r = random() & 0xFF;
    uint8_t g = random() & 0xFF; 
    uint8_t b = random() & 0xFF;
    return (r << 16) | (g << 8) | b;
}

void print_message(const char *msg) {
    while (*msg) {
        serial_putc(*msg++);
    }
}

void _start(void) {
    // Print startup message
    print_message(message);
    
    // Get screen dimensions
    uint32_t screen_width = fb_get_width();
    uint32_t screen_height = fb_get_height();
    
    // Print screen info to serial
    print_message("Screen size: ");
    // Simple number to string for width
    uint32_t w = screen_width;
    char width_str[16];
    int i = 0;
    if (w == 0) width_str[i++] = '0';
    else {
        char temp[16];
        int j = 0;
        while (w > 0) {
            temp[j++] = '0' + (w % 10);
            w /= 10;
        }
        while (j > 0) {
            width_str[i++] = temp[--j];
        }
    }
    width_str[i] = '\0';
    print_message(width_str);
    
    print_message("x");
    
    // Simple number to string for height
    uint32_t h = screen_height;
    char height_str[16];
    i = 0;
    if (h == 0) height_str[i++] = '0';
    else {
        char temp[16];
        int j = 0;
        while (h > 0) {
            temp[j++] = '0' + (h % 10);
            h /= 10;
        }
        while (j > 0) {
            height_str[i++] = temp[--j];
        }
    }
    height_str[i] = '\0';
    print_message(height_str);
    print_message("\r\n");
    
    // Clear screen with black
    fillrect(0x000000, 0, 0, screen_width, screen_height);
    
    // Draw random boxes
    for (int box = 0; box < 5000; box++) {
        // Random position and size
        uint32_t max_width = screen_width / 4;  // Max 1/4 screen width
        uint32_t max_height = screen_height / 4; // Max 1/4 screen height
        
        uint32_t width = random_range(20, max_width);
        uint32_t height = random_range(20, max_height);
        uint32_t x = random_range(0, screen_width - width);
        uint32_t y = random_range(0, screen_height - height);
        
        // Random color
        uint32_t color = random_color();
        
        // Draw the rectangle
        fillrect(color, x, y, width, height);
        
        // Small delay (busy wait)
        for (volatile int delay = 0; delay < 10000000; delay++) {
            __asm__ volatile ("nop");
        }
        
        counter++;
        
    }
    
    print_message("Finished drawing 50 random boxes!\r\n");
    
    // Infinite loop 
    while (1) {
        __asm__ volatile ("nop");
    }
} 