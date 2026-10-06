#include <stdint.h>

// Math constants
#define PI 3.14159265359f
#define DEG_TO_RAD (PI / 180.0f)

// syscall numbers
#define SYS_OPEN  3
#define SYS_CLOSE 4
#define SYS_READ  1
#define SYS_WRITE 2
#define SYS_FB_CLEAR    63
#define SYS_FB_FILLRECT 64
#define SYS_FB_GET_WIDTH 65
#define SYS_FB_GET_HEIGHT 66
#define SYS_KB_READ      67
#define SYS_GET_TERMINATE_FLAG 69
#define O_RDONLY  0x00
#define O_WRONLY  0x01
#define O_CREAT   0x04
#define O_TRUNC   0x08

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

// Function pointers to kernel functions (addresses from kernel.sym)
static void clear_screen(void) { syscall(SYS_FB_CLEAR, 0, 0, 0, 0, 0); }
static uint64_t fb_get_width(void) { return syscall(SYS_FB_GET_WIDTH, 0, 0, 0, 0, 0); }
static uint64_t fb_get_height(void) { return syscall(SYS_FB_GET_HEIGHT, 0, 0, 0, 0, 0); }
static void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    syscall(SYS_FB_FILLRECT, color, x, y, w, h);
};
// Double buffering variables
static uint32_t *back_buffer = 0;
static uint64_t buffer_width = 0;
static uint64_t buffer_height = 0;
static uint64_t pixels_per_line = 0;

// Simple delay function
void delay(uint32_t count) {
    for (volatile uint32_t i = 0; i < count * 30000; i++) {
        __asm__ volatile ("nop");
    }
}

// Generate RGB color
uint32_t make_color(uint8_t r, uint8_t g, uint8_t b) {
    return (r << 16) | (g << 8) | b;
}

// Simple memory allocation (static buffer)
static uint8_t memory_pool[1920 * 1080 * 4];  // Max 1920x1080 32-bit buffer
static uint64_t memory_offset = 0;

void* simple_malloc(uint64_t size) {
    if (memory_offset + size > sizeof(memory_pool)) {
        return 0;  // Out of memory
    }
    void* ptr = &memory_pool[memory_offset];
    memory_offset += size;
    return ptr;
}

// Initialize double buffering
int init_double_buffer() {
    buffer_width = fb_get_width();
    buffer_height = fb_get_height();
    
    if (buffer_width == 0 || buffer_height == 0) {
        return 0;  // Failed
    }
    
    // Assume 32-bit pixels (4 bytes per pixel)
    pixels_per_line = buffer_width;
    uint64_t buffer_size = buffer_width * buffer_height * 4;
    
    back_buffer = (uint32_t*)simple_malloc(buffer_size);
    
    if (!back_buffer) {
        return 0;  // Failed to allocate
    }
    
    return 1;  // Success
}

// Clear back buffer to specified color
void clear_back_buffer(uint32_t color) {
    if (!back_buffer) return;
    
    for (uint64_t i = 0; i < buffer_width * buffer_height; i++) {
        back_buffer[i] = color;
    }
}

// Draw pixel to back buffer
void putpixel_buffer(uint64_t x, uint64_t y, uint32_t color) {
    if (!back_buffer) return;
    if (x >= buffer_width || y >= buffer_height) return;
    
    back_buffer[y * pixels_per_line + x] = color;
}

// Draw rectangle to back buffer
void fillrect_buffer(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!back_buffer) return;
    
    // Bounds checking and clipping
    if (x >= buffer_width || y >= buffer_height) return;
    if (x + w > buffer_width) w = buffer_width - x;
    if (y + h > buffer_height) h = buffer_height - y;
    
    for (uint64_t j = 0; j < h; j++) {
        for (uint64_t i = 0; i < w; i++) {
            back_buffer[(y + j) * pixels_per_line + (x + i)] = color;
        }
    }
}

// Copy back buffer to front buffer (swap buffers)
void swap_buffers() {
    if (!back_buffer) return;
    
    // Optimized approach: copy lines at a time by finding consecutive pixels of same color
    for (uint64_t y = 0; y < buffer_height; y++) {
        uint64_t x = 0;
        while (x < buffer_width) {
            uint32_t color = back_buffer[y * pixels_per_line + x];
            uint64_t run_length = 1;
            
            // Find consecutive pixels of the same color
            while ((x + run_length < buffer_width) && 
                   (back_buffer[y * pixels_per_line + x + run_length] == color)) {
                run_length++;
            }
            
            // Draw the run of pixels with one fillrect call
            fillrect(color, x, y, run_length, 1);
            x += run_length;
        }
    }
}

// 3D Vector structure
typedef struct {
    float x, y, z;
} Vector3;

// 2D Point structure
typedef struct {
    int x, y;
} Point2D;

// Simple math functions
float sin_approx(float x) {
    // Simple sine approximation using Taylor series (first few terms)
    // Normalize to -PI to PI range
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    
    float x2 = x * x;
    return x - (x * x2) / 6.0f + (x * x2 * x2) / 120.0f;
}

float cos_approx(float x) {
    // cos(x) = sin(x + PI/2)
    return sin_approx(x + PI / 2.0f);
}

// 3D rotation around Y axis
Vector3 rotate_y(Vector3 v, float angle) {
    float cos_a = cos_approx(angle);
    float sin_a = sin_approx(angle);
    
    Vector3 result;
    result.x = v.x * cos_a + v.z * sin_a;
    result.y = v.y;
    result.z = -v.x * sin_a + v.z * cos_a;
    
    return result;
}

// 3D rotation around X axis
Vector3 rotate_x(Vector3 v, float angle) {
    float cos_a = cos_approx(angle);
    float sin_a = sin_approx(angle);
    
    Vector3 result;
    result.x = v.x;
    result.y = v.y * cos_a - v.z * sin_a;
    result.z = v.y * sin_a + v.z * cos_a;
    
    return result;
}

// 3D rotation around Z axis
Vector3 rotate_z(Vector3 v, float angle) {
    float cos_a = cos_approx(angle);
    float sin_a = sin_approx(angle);
    
    Vector3 result;
    result.x = v.x * cos_a - v.y * sin_a;
    result.y = v.x * sin_a + v.y * cos_a;
    result.z = v.z;
    
    return result;
}

// Project 3D point to 2D screen coordinates
Point2D project_3d_to_2d(Vector3 v, uint64_t screen_w, uint64_t screen_h) {
    float distance = 4.0f;  // Distance from camera to projection plane
    float scale = 100.0f;   // Scale factor
    
    Point2D result;
    result.x = (int)(screen_w / 2 + (v.x * scale * distance) / (v.z + distance));
    result.y = (int)(screen_h / 2 - (v.y * scale * distance) / (v.z + distance));
    
    return result;
}

// Draw a line to back buffer using Bresenham's algorithm
void draw_line(Point2D p1, Point2D p2, uint32_t color) {
    int x0 = p1.x, y0 = p1.y;
    int x1 = p2.x, y1 = p2.y;
    
    int dx = x1 - x0;
    int dy = y1 - y0;
    
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        // Draw a thicker line (3x3 pixels) for better visibility
        fillrect_buffer(color, x0-1, y0-1, 3, 3);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void _start(void) {
    uint64_t screen_width = fb_get_width();
    uint64_t screen_height = fb_get_height();
    
    // Initialize double buffering
    if (!init_double_buffer()) {
        // Fallback to single buffering if double buffer fails
        clear_screen();
        return;
    }
    
    // Define cube vertices (8 corners of a cube)
    Vector3 cube_vertices[8] = {
        {-1.0f, -1.0f, -1.0f},  // 0: back-bottom-left
        { 1.0f, -1.0f, -1.0f},  // 1: back-bottom-right
        { 1.0f,  1.0f, -1.0f},  // 2: back-top-right
        {-1.0f,  1.0f, -1.0f},  // 3: back-top-left
        {-1.0f, -1.0f,  1.0f},  // 4: front-bottom-left
        { 1.0f, -1.0f,  1.0f},  // 5: front-bottom-right
        { 1.0f,  1.0f,  1.0f},  // 6: front-top-right
        {-1.0f,  1.0f,  1.0f}   // 7: front-top-left
    };
    
    // Define cube edges (which vertices connect to which)
    int cube_edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},  // back face
        {4, 5}, {5, 6}, {6, 7}, {7, 4},  // front face
        {0, 4}, {1, 5}, {2, 6}, {3, 7}   // connecting edges
    };
    
    float angle_x = 0.0f;
    float angle_y = 0.0f;
    float angle_z = 0.0f;
    
    // Colors for different faces
    uint32_t colors[3] = {
        make_color(255, 100, 100),  // Red-ish
        make_color(100, 255, 100),  // Green-ish
        make_color(100, 100, 255)   // Blue-ish
    };
    
    // Main animation loop with double buffering
    for (int frame = 0; frame < 10000000000000; frame++) {
        // Clear back buffer to black
        clear_back_buffer(0x000000);
        
        // Rotate the cube
        Vector3 rotated_vertices[8];
        for (int i = 0; i < 8; i++) {
            Vector3 v = cube_vertices[i];
            v = rotate_x(v, angle_x);
            v = rotate_y(v, angle_y);
            v = rotate_z(v, angle_z);
            rotated_vertices[i] = v;
        }
        
        // Project vertices to 2D
        Point2D projected_vertices[8];
        for (int i = 0; i < 8; i++) {
            projected_vertices[i] = project_3d_to_2d(rotated_vertices[i], screen_width, screen_height);
        }
        
        // Draw all edges to back buffer
        for (int i = 0; i < 12; i++) {
            int v1 = cube_edges[i][0];
            int v2 = cube_edges[i][1];
            
            // Choose color based on edge type
            uint32_t edge_color;
            if (i < 4) {
                edge_color = colors[0];  // Back face edges
            } else if (i < 8) {
                edge_color = colors[1];  // Front face edges
            } else {
                edge_color = colors[2];  // Connecting edges
            }
            
            draw_line(projected_vertices[v1], projected_vertices[v2], edge_color);
        }
        
        // Swap buffers - copy back buffer to front buffer in one atomic operation
        swap_buffers();
        
        // Update rotation angles (slower rotation for smoother appearance)
        angle_x += 0.015f;
        angle_y += 0.02f;
        angle_z += 0.008f;
        
        // Keep angles in reasonable range
        if (angle_x > 2 * PI) angle_x -= 2 * PI;
        if (angle_y > 2 * PI) angle_y -= 2 * PI;
        if (angle_z > 2 * PI) angle_z -= 2 * PI;
        
        delay(10);  // Reduced delay since double buffering eliminates flicker
    }
    // Clean exit
    clear_screen();
}
