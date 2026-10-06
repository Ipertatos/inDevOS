#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// Simple test program for VMware SVGA-II driver
// This will be integrated into the kernel or can be run as a user-space test

// Include the VMware SVGA driver functions
extern bool vmware_svga_is_available(void);
extern void vmware_svga_get_info(uint32_t *width, uint32_t *height);
extern void vmware_svga_clear_screen(uint32_t color);
extern void vmware_svga_fillrect(uint32_t color, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
extern void vmware_svga_rect_copy(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);
extern int vmware_svga_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
extern void vmware_svga_driver_adapter_status(void);

void vmware_svga_test_basic_operations(void) {
    printf("=== VMware SVGA-II Driver Test ===\n");
    
    // Check if driver is available
    if (!vmware_svga_is_available()) {
        printf("VMware SVGA driver is not available\n");
        return;
    }
    
    printf("VMware SVGA driver is available!\n");
    
    // Get current display info
    uint32_t width, height;
    vmware_svga_get_info(&width, &height);
    printf("Current display mode: %dx%d\n", width, height);
    
    // Display driver status
    vmware_svga_driver_adapter_status();
    
    printf("\n=== Running Graphics Tests ===\n");
    
    // Test 1: Clear screen to blue
    printf("Test 1: Clearing screen to blue...\n");
    vmware_svga_clear_screen(0x000066CC);  // Blue
    
    // Small delay simulation (in real kernel, we'd use proper timing)
    for (volatile int i = 0; i < 1000000; i++);
    
    // Test 2: Draw some colored rectangles
    printf("Test 2: Drawing colored rectangles...\n");
    
    // Red rectangle
    vmware_svga_fillrect(0x00FF0000, 50, 50, 200, 100);
    
    // Green rectangle
    vmware_svga_fillrect(0x0000FF00, 300, 100, 150, 120);
    
    // Blue rectangle
    vmware_svga_fillrect(0x000000FF, 150, 200, 180, 80);
    
    // Yellow rectangle
    vmware_svga_fillrect(0x00FFFF00, 400, 250, 100, 100);
    
    // Another delay
    for (volatile int i = 0; i < 2000000; i++);
    
    // Test 3: Rectangle copy test (if supported)
    printf("Test 3: Testing rectangle copy...\n");
    vmware_svga_rect_copy(50, 50, 500, 400, 200, 100);  // Copy red rectangle
    
    // Test 4: Different colors and patterns
    printf("Test 4: Drawing gradient-like pattern...\n");
    for (int i = 0; i < 50; i++) {
        uint32_t color = (i * 5) << 16 | ((50-i) * 5) << 8 | 128;  // Gradient colors
        vmware_svga_fillrect(color, 600 + i * 2, 100 + i, 4, 4);
    }
    
    // Test 5: Mode switching test (be careful with this)
    printf("Test 5: Testing mode switch to 800x600...\n");
    if (vmware_svga_set_mode(800, 600, 32) == 0) {
        printf("Mode switch successful\n");
        
        // Clear to a different color in new mode
        vmware_svga_clear_screen(0x00336633);  // Dark green
        
        // Draw a test pattern
        for (int y = 0; y < 600; y += 50) {
            for (int x = 0; x < 800; x += 50) {
                uint32_t color = ((x/50) % 2) ^ ((y/50) % 2) ? 0x00FFFFFF : 0x00000000;
                vmware_svga_fillrect(color, x, y, 25, 25);
            }
        }
        
        // Switch back to original mode
        printf("Switching back to %dx%d...\n", width, height);
        vmware_svga_set_mode(width, height, 32);
    } else {
        printf("Mode switch failed\n");
    }
    
    // Final test: Clear to original color
    printf("Test complete: Clearing to final color...\n");
    vmware_svga_clear_screen(0x00000000);  // Black
    
    printf("=== VMware SVGA-II Driver Test Complete ===\n");
}

void vmware_svga_test_performance(void) {
    printf("\n=== VMware SVGA-II Performance Test ===\n");
    
    if (!vmware_svga_is_available()) {
        printf("VMware SVGA driver is not available\n");
        return;
    }
    
    uint32_t width, height;
    vmware_svga_get_info(&width, &height);
    
    printf("Testing fillrect performance...\n");
    
    // Performance test: many small rectangles
    uint64_t start_time = 0; // In real implementation, use HPET or TSC
    
    for (int i = 0; i < 1000; i++) {
        uint32_t x = (i * 37) % (width - 50);
        uint32_t y = (i * 73) % (height - 50);
        uint32_t color = (i * 0x123456) & 0x00FFFFFF;
        vmware_svga_fillrect(color, x, y, 20, 20);
    }
    
    uint64_t end_time = 0; // In real implementation, use HPET or TSC
    
    printf("Drew 1000 rectangles\n");
    // printf("Time taken: %llu ticks\n", end_time - start_time);
    
    printf("=== Performance Test Complete ===\n");
}

// This function can be called from kernel space or integrated into a command
int vmware_svga_test_main(void) {
    vmware_svga_test_basic_operations();
    vmware_svga_test_performance();
    return 0;
}

// For kernel integration - this could be added as a command
void cmd_vmware_test(void) {
    vmware_svga_test_main();
}

