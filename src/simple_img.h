#ifndef SIMPLE_IMG_H
#define SIMPLE_IMG_H

#include <stdint.h>
#include <stdbool.h>

// Simple uncompressed image format
// Header: "SIMG" + width(4) + height(4) + format(1)
// Data: Raw pixel data

#define SIMPLE_IMG_MAGIC 0x474D4953  // "SIMG" in little endian

typedef enum {
    SIMPLE_IMG_RGB = 3,   // 3 bytes per pixel (RGB)
    SIMPLE_IMG_RGBA = 4   // 4 bytes per pixel (RGBA)
} simple_img_format_t;

typedef struct {
    uint32_t magic;     // Should be SIMPLE_IMG_MAGIC
    uint32_t width;
    uint32_t height;
    uint8_t format;     // simple_img_format_t
    uint8_t reserved[3]; // Padding to 16 bytes
} __attribute__((packed)) simple_img_header_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t format;
    uint32_t *pixels;   // RGBA8888 format (converted)
    bool valid;
} simple_img_t;

// Function prototypes
simple_img_t* simple_img_decode(const uint8_t *data, uint32_t size);
void simple_img_free(simple_img_t *img);
bool simple_img_validate_header(const uint8_t *data, uint32_t size);

#endif // SIMPLE_IMG_H

