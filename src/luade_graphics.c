#include "render.h"
#include "luade_graphics.h"
#include "utils.h"
#include "term.h"
#include "keyboard.h"
#include "mem.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "atomic.h"
#include "debug_config.h"
#include <stdint.h>

// Forward declaration for printf
int printf(const char *fmt, ...);

/* LuaDE Graphics API - Expose framebuffer operations to Lua */

extern struct limine_framebuffer *fb;

// Double buffering for LuaDE graphics
static uint32_t *luade_back_buffer = 0;
static uint64_t luade_buffer_width = 0;
static uint64_t luade_buffer_height = 0;
static uint64_t luade_pixels_per_line = 0;
static uint8_t luade_memory_pool[1920 * 1080 * 4];  // Max 1920x1080 32-bit buffer
static uint64_t luade_memory_offset = 0;

// Simple memory allocator for LuaDE
void* luade_malloc(uint64_t size) {
    // Add alignment to ensure 8-byte boundary
    size = (size + 7) & ~7;
    
    if (luade_memory_offset + size > sizeof(luade_memory_pool)) {
        printf("[luade] ERROR: Out of memory! Requested: %llu, Available: %llu\n", 
               size, sizeof(luade_memory_pool) - luade_memory_offset);
        return 0;  // Out of memory
    }
    void* ptr = &luade_memory_pool[luade_memory_offset];
    luade_memory_offset += size;
    printf("[luade] DEBUG: Allocated %llu bytes at %p\n", size, ptr);
    return ptr;
}

// Initialize LuaDE double buffering
int luade_init_double_buffer() {
    if (!fb) {
        printf("[luade] ERROR: Framebuffer not available\n");
        return 0;
    }
    
    luade_buffer_width = fb->width;
    luade_buffer_height = fb->height;
    
    printf("[luade] DEBUG: Screen dimensions: %llux%llu\n", luade_buffer_width, luade_buffer_height);
    
    if (luade_buffer_width == 0 || luade_buffer_height == 0) {
        printf("[luade] ERROR: Invalid screen dimensions\n");
        return 0;  // Failed
    }
    
    // Limit buffer size to reasonable bounds
    if (luade_buffer_width > 1920 || luade_buffer_height > 1080) {
        printf("[luade] WARNING: Large screen size, limiting to 1920x1080\n");
        luade_buffer_width = (luade_buffer_width > 1920) ? 1920 : luade_buffer_width;
        luade_buffer_height = (luade_buffer_height > 1080) ? 1080 : luade_buffer_height;
    }
    
    luade_pixels_per_line = luade_buffer_width;
    uint64_t buffer_size = luade_buffer_width * luade_buffer_height * 4;
    
    printf("[luade] DEBUG: Allocating %llu bytes for back buffer\n", buffer_size);
    
    luade_back_buffer = (uint32_t*)luade_malloc(buffer_size);
    
    if (!luade_back_buffer) {
        printf("[luade] ERROR: Failed to allocate back buffer\n");
        return 0;  // Failed to allocate
    }
    
    printf("[luade] DEBUG: Back buffer allocated at %p\n", luade_back_buffer);
    
    // Clear back buffer to black
    for (uint64_t i = 0; i < luade_buffer_width * luade_buffer_height; i++) {
        luade_back_buffer[i] = 0x000000;
    }
    
    printf("[luade] SUCCESS: Double buffering initialized\n");
    return 1;  // Success
}

// Clear LuaDE back buffer to specified color
void luade_clear_back_buffer(uint32_t color) {
    if (!luade_back_buffer) return;
    
    for (uint64_t i = 0; i < luade_buffer_width * luade_buffer_height; i++) {
        luade_back_buffer[i] = color;
    }
}

// Draw pixel to LuaDE back buffer
void luade_putpixel_buffer(uint64_t x, uint64_t y, uint32_t color) {
    if (!luade_back_buffer) return;
    if (x >= luade_buffer_width || y >= luade_buffer_height) return;
    
    luade_back_buffer[y * luade_pixels_per_line + x] = color;
}

// Draw rectangle to LuaDE back buffer
void luade_fillrect_buffer(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!luade_back_buffer) return;
    
    // Bounds checking and clipping
    if (x >= luade_buffer_width || y >= luade_buffer_height) return;
    if (x + w > luade_buffer_width) w = luade_buffer_width - x;
    if (y + h > luade_buffer_height) h = luade_buffer_height - y;
    
    for (uint64_t j = 0; j < h; j++) {
        for (uint64_t i = 0; i < w; i++) {
            luade_back_buffer[(y + j) * luade_pixels_per_line + (x + i)] = color;
        }
    }
}

bool luade_has_back_buffer(void) { return luade_back_buffer != 0; }

// Copy LuaDE back buffer to front buffer (swap buffers)
void luade_swap_buffers() {
    if (!luade_back_buffer) return;
    
    // Optimized approach: copy lines at a time by finding consecutive pixels of same color
    for (uint64_t y = 0; y < luade_buffer_height; y++) {
        uint64_t x = 0;
        while (x < luade_buffer_width) {
            uint32_t color = luade_back_buffer[y * luade_pixels_per_line + x];
            uint64_t run_length = 1;
            
            // Find consecutive pixels of the same color
            while ((x + run_length < luade_buffer_width) && 
                   (luade_back_buffer[y * luade_pixels_per_line + x + run_length] == color)) {
                run_length++;
            }
            
            // Draw the run of pixels with one fillrect call
            fillrect(color, x, y, run_length, 1);
            x += run_length;
        }
    }
}

// Lua function: gfx.get_screen_size() -> width, height
static int l_get_screen_size(lua_State *L) {
    printf("[gfx] DEBUG: Entering gfx.get_screen_size()\n");

    if (!fb) {
        printf("[gfx] DEBUG: fb is NULL, using fallback\n");
        lua_pushinteger(L, 80);   // fallback to VGA text dimensions
        lua_pushinteger(L, 25);
        return 2;
    }

    printf("[gfx] DEBUG: fb pointer is: %p\n", fb);

    // Validate framebuffer pointer sanity before touching fields
    if ((uintptr_t)fb < 0x1000 || (uintptr_t)fb == 0xdeadbeef || (uintptr_t)fb == 0xffffffff) {
        printf("[gfx] ERROR: Corrupted framebuffer pointer: %p\n", fb);
        lua_pushinteger(L, 80);   // fallback to VGA text dimensions
        lua_pushinteger(L, 25);
        return 2;
    }

    // Read framebuffer dimensions under interrupts-disabled critical section
    uint64_t width = 0;
    uint64_t height = 0;

    printf("[gfx] DEBUG: fb pointer validation passed, about to access fb->width\n");
    uint64_t flags = irq_save();
    printf("[gfx] DEBUG: About to read fb->width\n");
    width = fb->width;
    printf("[gfx] DEBUG: Successfully read width: %llu\n", width);
    printf("[gfx] DEBUG: About to read fb->height\n");
    height = fb->height;
    printf("[gfx] DEBUG: Successfully read height: %llu\n", height);
    irq_restore(flags);

    // Basic sanity on dimensions
    if (width == 0 || width > 10000 || height == 0 || height > 10000) {
        printf("[gfx] ERROR: Invalid framebuffer dimensions: %llux%llu\n", width, height);
        lua_pushinteger(L, 80);
        lua_pushinteger(L, 25);
        return 2;
    }

    printf("[gfx] DEBUG: Dimensions validation passed, returning %llux%llu\n", width, height);
    // Push as integers; avoid any other Lua calls here
    lua_pushinteger(L, (lua_Integer)width);
    lua_pushinteger(L, (lua_Integer)height);
    return 2;
}

// Lua function: gfx.put_pixel(x, y, color) - Now with automatic double buffering
static int l_put_pixel(lua_State *L) {
    int x = 0, y = 0, color = 0; 
    int isnum = 0;
    lua_Integer tmp;

    tmp = lua_tointegerx(L, 1, &isnum); if (!isnum) return 0; x = (int)tmp;
    tmp = lua_tointegerx(L, 2, &isnum); if (!isnum) return 0; y = (int)tmp;
    tmp = lua_tointegerx(L, 3, &isnum); if (!isnum) return 0; color = (int)tmp;

    if (x >= 0 && y >= 0) {
        // Use double buffering if available, otherwise fall back to direct rendering
        if (luade_back_buffer) {
            luade_putpixel_buffer((uint64_t)x, (uint64_t)y, (uint32_t)color);
        } else {
            uint64_t flags = irq_save();
            putpixel((uint64_t)x, (uint64_t)y, (uint32_t)color);
            irq_restore(flags);
        }
    }
    return 0;
}

// Lua function: gfx.fill_rect(x, y, w, h, color) - Now with automatic double buffering
static int l_fill_rect(lua_State *L) {
    // Defensive: only accept strict integers; avoid raising errors from Lua C API
    int x = 0, y = 0, w = 0, h = 0, color = 0;

    // Read Lua arguments without disabling interrupts; no kernel state touched yet
    if (!lua_isinteger(L, 1) || !lua_isinteger(L, 2) ||
        !lua_isinteger(L, 3) || !lua_isinteger(L, 4) ||
        !lua_isinteger(L, 5)) {
        return 0;
    }
    x = (int)lua_tointeger(L, 1);
    y = (int)lua_tointeger(L, 2);
    w = (int)lua_tointeger(L, 3);
    h = (int)lua_tointeger(L, 4);
    color = (int)lua_tointeger(L, 5);

    if (x >= 0 && y >= 0 && w > 0 && h > 0) {
        // Use double buffering if available, otherwise fall back to direct rendering
        if (luade_back_buffer) {
            luade_fillrect_buffer((uint32_t)color, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h);
        } else {
            uint64_t flags = irq_save();
            fillrect((uint32_t)color, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h);
            irq_restore(flags);
        }
    }
    return 0;
}

// Lua function: gfx.clear_screen(color) - Now with automatic double buffering
static int l_clear_screen(lua_State *L) {
    int color = 0x000000; // default black
    int isnum = 0;
    if (lua_gettop(L) >= 1) {
        lua_Integer tmp = lua_tointegerx(L, 1, &isnum);
        if (isnum) color = (int)tmp;
    }

    // Use double buffering if available, otherwise fall back to direct rendering
    if (luade_back_buffer) {
        luade_clear_back_buffer((uint32_t)color);
    } else {
        uint64_t flags = irq_save();
        if (fb) {
            fillrect((uint32_t)color, 0, 0, fb->width, fb->height);
        }
        irq_restore(flags);
    }
    return 0;
}

// Lua function: gfx.rgb(r, g, b) -> color
static int l_rgb(lua_State *L) {
    int isnum = 0;
    lua_Integer vr = lua_tointegerx(L, 1, &isnum); if (!isnum) { lua_pushinteger(L, 0); return 1; }
    lua_Integer vg = lua_tointegerx(L, 2, &isnum); if (!isnum) { lua_pushinteger(L, 0); return 1; }
    lua_Integer vb = lua_tointegerx(L, 3, &isnum); if (!isnum) { lua_pushinteger(L, 0); return 1; }
    int r = (int)vr & 0xFF;
    int g = (int)vg & 0xFF;
    int b = (int)vb & 0xFF;
    
    uint32_t color = (r << 16) | (g << 8) | b;
    lua_pushinteger(L, color);
    return 1;
}

// Bitmap font not wired; no framebuffer draw. Set DEBUG_LUA=1 in debug_config.h to trace calls.
// Lua function: gfx.draw_text(x, y, text, color)
static int l_draw_text(lua_State *L) {
    int isnum = 0;
    lua_Integer tmp;
    tmp = lua_tointegerx(L, 1, &isnum); if (!isnum) return 0; int x = (int)tmp;
    tmp = lua_tointegerx(L, 2, &isnum); if (!isnum) return 0; int y = (int)tmp;
    size_t tlen = 0; const char *text = lua_tolstring(L, 3, &tlen); if (!text) return 0;
    int color = 0xFFFFFF; // default white
    if (lua_gettop(L) >= 4) { tmp = lua_tointegerx(L, 4, &isnum); if (isnum) color = (int)tmp; }

    DEBUG_PRINT(LUA, "[LuaDE] gfx.draw_text (%d,%d) '%.*s' color=0x%x\n",
                 x, y, (int)tlen, text, color);
    r_target_t t;
    if (luade_back_buffer) {
        r_target_init(&t, luade_back_buffer, (int)luade_buffer_width,
                      (int)luade_buffer_height, (int)luade_pixels_per_line);
    } else {
        extern uint32_t *fb_draw_target(void);
        uint32_t *px = fb_draw_target();
        if (!px || fb->bpp != 32) return 0;
        r_target_init(&t, px, (int)fb->width,
                      (int)fb->height, (int)(fb->pitch / 4));
    }
    r_text(&t, x, y, text, 0xFF000000u | (uint32_t)color);
    if (!luade_back_buffer && y < (int)fb->height) {
        extern void fb_mark_dirty_rows(uint32_t y, uint32_t h);
        fb_mark_dirty_rows((uint32_t)(y < 0 ? 0 : y), 16);
    }
    return 0;
}

// Lua function: gfx.draw_line(x1, y1, x2, y2, color) - Now with automatic double buffering
static int l_draw_line(lua_State *L) {
    int isnum = 0; lua_Integer tmp;
    tmp = lua_tointegerx(L, 1, &isnum); if (!isnum) return 0; int x1 = (int)tmp;
    tmp = lua_tointegerx(L, 2, &isnum); if (!isnum) return 0; int y1 = (int)tmp;
    tmp = lua_tointegerx(L, 3, &isnum); if (!isnum) return 0; int x2 = (int)tmp;
    tmp = lua_tointegerx(L, 4, &isnum); if (!isnum) return 0; int y2 = (int)tmp;
    tmp = lua_tointegerx(L, 5, &isnum); if (!isnum) return 0; int color = (int)tmp;
    
    // Bresenham line algorithm
    int dx = x2 - x1;
    int dy = y2 - y1;
    int dx_abs = dx < 0 ? -dx : dx;
    int dy_abs = dy < 0 ? -dy : dy;
    int x_inc = dx < 0 ? -1 : 1;
    int y_inc = dy < 0 ? -1 : 1;
    int error = dx_abs - dy_abs;
    
    int x = x1, y = y1;
    while (1) {
        // Use double buffering if available, otherwise fall back to direct rendering
        if (luade_back_buffer) {
            luade_putpixel_buffer(x, y, color);
        } else {
            uint64_t flags = irq_save();
            putpixel(x, y, color);
            irq_restore(flags);
        }
        
        if (x == x2 && y == y2) break;
        
        int error2 = error * 2;
        if (error2 > -dy_abs) {
            error -= dy_abs;
            x += x_inc;
        }
        if (error2 < dx_abs) {
            error += dx_abs;
            y += y_inc;
        }
    }
    return 0;
}

// === DOUBLE BUFFER LUA FUNCTIONS ===

// Lua function: gfx.init_double_buffer() -> success
static int l_init_double_buffer(lua_State *L) {
    int success = luade_init_double_buffer();
    lua_pushboolean(L, success);
    return 1;
}

// Lua function: gfx.put_pixel_buffered(x, y, color)
static int l_put_pixel_buffered(lua_State *L) {
    int x = 0, y = 0, color = 0; 
    int isnum = 0;
    lua_Integer tmp;

    tmp = lua_tointegerx(L, 1, &isnum); if (!isnum) return 0; x = (int)tmp;
    tmp = lua_tointegerx(L, 2, &isnum); if (!isnum) return 0; y = (int)tmp;
    tmp = lua_tointegerx(L, 3, &isnum); if (!isnum) return 0; color = (int)tmp;

    if (x >= 0 && y >= 0) {
        luade_putpixel_buffer((uint64_t)x, (uint64_t)y, (uint32_t)color);
    }
    return 0;
}

// Lua function: gfx.fill_rect_buffered(x, y, w, h, color)
static int l_fill_rect_buffered(lua_State *L) {
    int x = 0, y = 0, w = 0, h = 0, color = 0;

    if (!lua_isinteger(L, 1) || !lua_isinteger(L, 2) ||
        !lua_isinteger(L, 3) || !lua_isinteger(L, 4) ||
        !lua_isinteger(L, 5)) {
        return 0;
    }
    x = (int)lua_tointeger(L, 1);
    y = (int)lua_tointeger(L, 2);
    w = (int)lua_tointeger(L, 3);
    h = (int)lua_tointeger(L, 4);
    color = (int)lua_tointeger(L, 5);

    if (x >= 0 && y >= 0 && w > 0 && h > 0) {
        luade_fillrect_buffer((uint32_t)color, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h);
    }
    return 0;
}

// Lua function: gfx.clear_screen_buffered(color)
static int l_clear_screen_buffered(lua_State *L) {
    int color = 0x000000; // default black
    int isnum = 0;
    if (lua_gettop(L) >= 1) {
        lua_Integer tmp = lua_tointegerx(L, 1, &isnum);
        if (isnum) color = (int)tmp;
    }

    luade_clear_back_buffer((uint32_t)color);
    return 0;
}

// Lua function: gfx.draw_line_buffered(x1, y1, x2, y2, color)
static int l_draw_line_buffered(lua_State *L) {
    int isnum = 0; lua_Integer tmp;
    tmp = lua_tointegerx(L, 1, &isnum); if (!isnum) return 0; int x1 = (int)tmp;
    tmp = lua_tointegerx(L, 2, &isnum); if (!isnum) return 0; int y1 = (int)tmp;
    tmp = lua_tointegerx(L, 3, &isnum); if (!isnum) return 0; int x2 = (int)tmp;
    tmp = lua_tointegerx(L, 4, &isnum); if (!isnum) return 0; int y2 = (int)tmp;
    tmp = lua_tointegerx(L, 5, &isnum); if (!isnum) return 0; int color = (int)tmp;
    
    // Bresenham line algorithm for back buffer
    int dx = x2 - x1;
    int dy = y2 - y1;
    int dx_abs = dx < 0 ? -dx : dx;
    int dy_abs = dy < 0 ? -dy : dy;
    int x_inc = dx < 0 ? -1 : 1;
    int y_inc = dy < 0 ? -1 : 1;
    int error = dx_abs - dy_abs;
    
    int x = x1, y = y1;
    while (1) {
        luade_putpixel_buffer(x, y, color);
        if (x == x2 && y == y2) break;
        
        int error2 = error * 2;
        if (error2 > -dy_abs) {
            error -= dy_abs;
            x += x_inc;
        }
        if (error2 < dx_abs) {
            error += dx_abs;
            y += y_inc;
        }
    }
    return 0;
}

// Lua function: gfx.swap_buffers()
static int l_swap_buffers(lua_State *L) {
    (void)L; // Unused parameter
    luade_swap_buffers();
    return 0;
}

// Register graphics functions with Lua
void luade_register_graphics(lua_State *L) {
    printf("[gfx] DEBUG: Starting graphics registration\n");
    
    // Check framebuffer status
    printf("[gfx] DEBUG: Framebuffer status - fb pointer: %p\n", fb);
    if (fb) {
        printf("[gfx] DEBUG: Framebuffer seems valid, dimensions: %llux%llu\n", fb->width, fb->height);
    } else {
        printf("[gfx] DEBUG: Framebuffer is NULL!\n");
    }
    
    // Create gfx table
    lua_newtable(L);
    printf("[gfx] DEBUG: Created gfx table\n");
    
    // Register functions
    printf("[gfx] DEBUG: About to register get_screen_size function\n");
    lua_pushcfunction(L, l_get_screen_size);
    printf("[gfx] DEBUG: Pushed l_get_screen_size function\n");
    lua_setfield(L, -2, "get_screen_size");
    printf("[gfx] DEBUG: Set get_screen_size field\n");
    
    lua_pushcfunction(L, l_put_pixel);
    lua_setfield(L, -2, "put_pixel");
    
    lua_pushcfunction(L, l_fill_rect);
    lua_setfield(L, -2, "fill_rect");
    
    lua_pushcfunction(L, l_clear_screen);
    lua_setfield(L, -2, "clear_screen");
    
    lua_pushcfunction(L, l_rgb);
    lua_setfield(L, -2, "rgb");
    
    lua_pushcfunction(L, l_draw_text);
    lua_setfield(L, -2, "draw_text");
    
    lua_pushcfunction(L, l_draw_line);
    lua_setfield(L, -2, "draw_line");
    
    // Double buffer functions
    lua_pushcfunction(L, l_init_double_buffer);
    lua_setfield(L, -2, "init_double_buffer");
    
    lua_pushcfunction(L, l_put_pixel_buffered);
    lua_setfield(L, -2, "put_pixel_buffered");
    
    lua_pushcfunction(L, l_fill_rect_buffered);
    lua_setfield(L, -2, "fill_rect_buffered");
    
    lua_pushcfunction(L, l_clear_screen_buffered);
    lua_setfield(L, -2, "clear_screen_buffered");
    
    lua_pushcfunction(L, l_draw_line_buffered);
    lua_setfield(L, -2, "draw_line_buffered");
    
    lua_pushcfunction(L, l_swap_buffers);
    lua_setfield(L, -2, "swap_buffers");
    
    // Color constants
    lua_pushinteger(L, 0x000000); lua_setfield(L, -2, "BLACK");
    lua_pushinteger(L, 0xFFFFFF); lua_setfield(L, -2, "WHITE");
    lua_pushinteger(L, 0xFF0000); lua_setfield(L, -2, "RED");
    lua_pushinteger(L, 0x00FF00); lua_setfield(L, -2, "GREEN");
    lua_pushinteger(L, 0x0000FF); lua_setfield(L, -2, "BLUE");
    lua_pushinteger(L, 0xFFFF00); lua_setfield(L, -2, "YELLOW");
    lua_pushinteger(L, 0xFF00FF); lua_setfield(L, -2, "MAGENTA");
    lua_pushinteger(L, 0x00FFFF); lua_setfield(L, -2, "CYAN");
    lua_pushinteger(L, 0x808080); lua_setfield(L, -2, "GRAY");
    lua_pushinteger(L, 0xC0C0C0); lua_setfield(L, -2, "LIGHT_GRAY");
    lua_pushinteger(L, 0x404040); lua_setfield(L, -2, "DARK_GRAY");
    
    // Set as global gfx
    printf("[gfx] DEBUG: About to set gfx as global\n");
    lua_setglobal(L, "gfx");
    printf("[gfx] DEBUG: Graphics registration complete\n");
}
