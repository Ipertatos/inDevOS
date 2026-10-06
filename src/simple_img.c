#include "simple_img.h"
#include "memory.h"
#include "mem.h"
#include "utils.h"
#include "string.h"

bool simple_img_validate_header(const uint8_t *data, uint32_t size) {
    if (!data || size < sizeof(simple_img_header_t)) {
        return false;
    }
    
    const simple_img_header_t *header = (const simple_img_header_t*)data;
    
    if (header->magic != SIMPLE_IMG_MAGIC) {
        return false;
    }
    
    if (header->format != SIMPLE_IMG_RGB && header->format != SIMPLE_IMG_RGBA) {
        printf("[SIMG] Invalid format: %u\n", header->format);
        return false;
    }
    
    if (header->width == 0 || header->height == 0 || 
        header->width > 2048 || header->height > 2048) {
        printf("[SIMG] Invalid dimensions: %ux%u\n", header->width, header->height);
        return false;
    }
    
    uint32_t expected_data_size = header->width * header->height * header->format;
    uint32_t total_size = sizeof(simple_img_header_t) + expected_data_size;
    
    if (size < total_size) {
        printf("[SIMG] File too small: %u < %u\n", size, total_size);
        return false;
    }
    
    return true;
}

simple_img_t* simple_img_decode(const uint8_t *data, uint32_t size) {
    if (!simple_img_validate_header(data, size)) {
        return NULL;
    }
    
    const simple_img_header_t *header = (const simple_img_header_t*)data;
    const uint8_t *pixel_data = data + sizeof(simple_img_header_t);
    
    printf("[SIMG] Decoding %ux%u image, format=%u\n", 
           header->width, header->height, header->format);
    
    simple_img_t *img = kalloc(sizeof(simple_img_t));
    if (!img) {
        printf("[SIMG] Failed to allocate image structure\n");
        return NULL;
    }
    
    img->width = header->width;
    img->height = header->height;
    img->format = header->format;
    img->valid = false;
    
    uint32_t pixel_count = img->width * img->height;
    img->pixels = kalloc(pixel_count * sizeof(uint32_t));
    
    if (!img->pixels) {
        printf("[SIMG] Failed to allocate pixel buffer\n");
        kfree(img);
        return NULL;
    }
    
    // Convert pixel data to RGBA8888
    for (uint32_t i = 0; i < pixel_count; i++) {
        uint8_t r, g, b, a = 255;
        
        if (header->format == SIMPLE_IMG_RGB) {
            r = pixel_data[i * 3 + 0];
            g = pixel_data[i * 3 + 1];
            b = pixel_data[i * 3 + 2];
        } else { // SIMPLE_IMG_RGBA
            r = pixel_data[i * 4 + 0];
            g = pixel_data[i * 4 + 1];
            b = pixel_data[i * 4 + 2];
            a = pixel_data[i * 4 + 3];
        }
        
        img->pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    
    img->valid = true;
    printf("[SIMG] Successfully decoded %ux%u image\n", img->width, img->height);
    return img;
}

void simple_img_free(simple_img_t *img) {
    if (!img) return;
    
    if (img->pixels) {
        kfree(img->pixels);
    }
    
    kfree(img);
}

