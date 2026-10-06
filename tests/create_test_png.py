#!/usr/bin/env python3
"""
Create a simple test PNG that's easier for our basic decoder to handle
"""

from PIL import Image
import numpy as np

def create_simple_test_image():
    # Create a simple 64x64 test pattern
    width, height = 64, 64
    
    # Create RGB array
    img_array = np.zeros((height, width, 3), dtype=np.uint8)
    
    # Create a simple pattern - red square in top-left, green in top-right, 
    # blue in bottom-left, white in bottom-right
    img_array[:height//2, :width//2] = [255, 0, 0]      # Red
    img_array[:height//2, width//2:] = [0, 255, 0]      # Green  
    img_array[height//2:, :width//2] = [0, 0, 255]      # Blue
    img_array[height//2:, width//2:] = [255, 255, 255]  # White
    
    # Create PIL image
    img = Image.fromarray(img_array, 'RGB')
    
    # Save with minimal compression
    img.save('test_simple.png', 'PNG', compress_level=0)
    print("Created test_simple.png")

def create_gradient_image():
    # Create a gradient test image
    width, height = 128, 128
    
    img_array = np.zeros((height, width, 3), dtype=np.uint8)
    
    for y in range(height):
        for x in range(width):
            img_array[y, x] = [
                int(255 * x / width),      # Red gradient left to right
                int(255 * y / height),     # Green gradient top to bottom  
                128                        # Constant blue
            ]
    
    img = Image.fromarray(img_array, 'RGB')
    img.save('test_gradient.png', 'PNG', compress_level=0)
    print("Created test_gradient.png")

if __name__ == "__main__":
    create_simple_test_image()
    create_gradient_image()

