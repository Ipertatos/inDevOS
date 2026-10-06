#ifndef PNG_H
#define PNG_H

#include <stdint.h>
#include <stdbool.h>

/* PNG Image Decoder Header */

// PNG signature bytes
#define PNG_SIGNATURE_SIZE 8
extern const uint8_t PNG_SIGNATURE[PNG_SIGNATURE_SIZE];

// PNG chunk types (big-endian)
#define PNG_IHDR 0x49484452  // Image header
#define PNG_IDAT 0x49444154  // Image data
#define PNG_IEND 0x49454E44  // Image end
#define PNG_PLTE 0x504C5445  // Palette

// PNG color types
#define PNG_COLOR_GRAYSCALE       0
#define PNG_COLOR_RGB             2
#define PNG_COLOR_PALETTE         3
#define PNG_COLOR_GRAYSCALE_ALPHA 4
#define PNG_COLOR_RGB_ALPHA       6

// PNG filter types
#define PNG_FILTER_NONE    0
#define PNG_FILTER_SUB     1
#define PNG_FILTER_UP      2
#define PNG_FILTER_AVERAGE 3
#define PNG_FILTER_PAETH   4

// PNG image structure
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t compression_method;
    uint8_t filter_method;
    uint8_t interlace_method;
    uint32_t *pixels;          // RGBA pixels (32-bit per pixel)
    bool valid;
} png_image_t;

// PNG chunk structure
typedef struct {
    uint32_t length;   // Data length (big-endian)
    uint32_t type;     // Chunk type (big-endian)
    uint8_t *data;     // Chunk data
    uint32_t crc;      // CRC checksum (big-endian)
} png_chunk_t;

// Function declarations
png_image_t* png_decode(const uint8_t *data, uint32_t size);
void png_free(png_image_t *image);
bool png_validate_signature(const uint8_t *data, uint32_t size);
uint32_t png_read_be32(const uint8_t *data);
uint16_t png_read_be16(const uint8_t *data);

// Simple decompression for uncompressed or basic PNG files.
// expected_size: exact inflate output size from IHDR if known, 0 if unknown.
int png_decompress_data(const uint8_t *compressed_data, uint32_t compressed_size,
                       uint8_t **decompressed_data, uint32_t *decompressed_size,
                       uint32_t expected_size);

// PNG filter functions
void png_apply_filters(uint8_t *image_data, uint32_t width, uint32_t height, 
                      uint8_t color_type, uint8_t bit_depth);

#endif // PNG_H

