-- LuaDE Double Buffered Test (Simple)
print("LuaDE Double Buffered Test")

-- Initialize double buffering
local success = gfx.init_double_buffer()
if not success then
    print("ERROR: Failed to initialize double buffering!")
    return
end

print("Double buffering initialized successfully")

-- Get screen dimensions
local width, height = gfx.get_screen_size()
print("Screen size: " .. width .. "x" .. height)

-- Simple math constants
local PI = 3.14159265359

-- Simple test animation - just drawing rectangles
print("Starting simple double buffer test...")

for i = 1, 50 do
    -- Clear back buffer
    gfx.clear_screen_buffered(gfx.BLACK)
    
    -- Draw some animated rectangles to back buffer
    local x = (i * 5) % (width - 100)
    local y = (i * 3) % (height - 100)
    
    gfx.fill_rect_buffered(x, y, 50, 50, gfx.RED)
    gfx.fill_rect_buffered(x + 60, y + 10, 30, 30, gfx.GREEN)
    gfx.fill_rect_buffered(x + 20, y + 60, 40, 20, gfx.BLUE)
    
    -- Draw some lines
    gfx.draw_line_buffered(x, y, x + 100, y + 100, gfx.WHITE)
    gfx.draw_line_buffered(x + 100, y, x, y + 100, gfx.YELLOW)
    
    -- Swap buffers to display
    gfx.swap_buffers()
end

print("Double buffer test complete!")
gfx.clear_screen(gfx.BLACK)
