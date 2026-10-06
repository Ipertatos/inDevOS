#include "png.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "string.h"

/* PNG decode is on the hot path during desktop wallpaper load.  Each
 * serial_printf blocks ~1ms at 115200 baud — multiplied by hundreds of
 * trace messages it freezes the main loop for seconds, blocking
 * mouse_poll() and the compositor.  Keep diagnostics behind a flag so
 * they can be re-enabled if the decoder ever needs debugging. */
#ifndef PNG_VERBOSE
#define PNG_VERBOSE 0
#endif
#if PNG_VERBOSE
#define png_debug(...) printf(__VA_ARGS__)
#else
#define png_debug(...) do { } while (0)
#endif

// PNG signature
const uint8_t PNG_SIGNATURE[PNG_SIGNATURE_SIZE] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
};

// Read big-endian 32-bit value
uint32_t png_read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           ((uint32_t)data[3]);
}

// Read big-endian 16-bit value
uint16_t png_read_be16(const uint8_t *data) {
    return ((uint16_t)data[0] << 8) | ((uint16_t)data[1]);
}

// Validate PNG signature
bool png_validate_signature(const uint8_t *data, uint32_t size) {
    if (size < PNG_SIGNATURE_SIZE) {
        return false;
    }
    return memcmp(data, PNG_SIGNATURE, PNG_SIGNATURE_SIZE) == 0;
}

// Simple Paeth predictor for PNG filtering
static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Apply PNG row filters
void png_apply_filters(uint8_t *image_data, uint32_t width, uint32_t height,
                      uint8_t color_type, uint8_t bit_depth) {
    if (!image_data) {
        png_debug("[PNG] NULL image_data in png_apply_filters\n");
        return;
    }

    uint32_t bytes_per_pixel;
    
    // Calculate bytes per pixel based on color type and bit depth
    switch (color_type) {
        case PNG_COLOR_GRAYSCALE:
            bytes_per_pixel = (bit_depth + 7) / 8;
            break;
        case PNG_COLOR_RGB:
            bytes_per_pixel = 3 * ((bit_depth + 7) / 8);
            break;
        case PNG_COLOR_PALETTE:
            bytes_per_pixel = (bit_depth + 7) / 8;
            break;
        case PNG_COLOR_GRAYSCALE_ALPHA:
            bytes_per_pixel = 2 * ((bit_depth + 7) / 8);
            break;
        case PNG_COLOR_RGB_ALPHA:
            bytes_per_pixel = 4 * ((bit_depth + 7) / 8);
            break;
        default:
            bytes_per_pixel = 1;
    }
    
    uint32_t row_bytes = width * bytes_per_pixel;
    uint32_t stride = row_bytes + 1; // +1 for filter byte
    
    for (uint32_t y = 0; y < height; y++) {
        // Check bounds to prevent page fault
        uint64_t row_offset = (uint64_t)y * stride;
        if (row_offset + stride > 1024 * 1024 * 10) { // Reasonable max size limit
            png_debug("[PNG] Row offset too large in filters: %lu\n", row_offset);
            return;
        }

        uint8_t *row = image_data + row_offset;
        uint8_t filter_type = row[0];
        uint8_t *pixels = row + 1;

        // Validate filter type
        if (filter_type > 4) {
            png_debug("[PNG] Invalid filter type %d at row %u, treating as NONE\n", filter_type, y);
            filter_type = PNG_FILTER_NONE; // Default to no filtering
        } else if (filter_type != PNG_FILTER_NONE) {
            png_debug("[PNG] Applying filter type %d to row %u\n", filter_type, y);
        }

        switch (filter_type) {
            case PNG_FILTER_NONE:
                // No filtering
                break;
                
            case PNG_FILTER_SUB:
                for (uint32_t x = bytes_per_pixel; x < row_bytes; x++) {
                    pixels[x] = (pixels[x] + pixels[x - bytes_per_pixel]) & 0xFF;
                }
                break;
                
            case PNG_FILTER_UP:
                if (y > 0) {
                    uint8_t *prev_pixels = image_data + (y - 1) * stride + 1;
                    for (uint32_t x = 0; x < row_bytes; x++) {
                        pixels[x] = (pixels[x] + prev_pixels[x]) & 0xFF;
                    }
                }
                break;
                
            case PNG_FILTER_AVERAGE:
                for (uint32_t x = 0; x < row_bytes; x++) {
                    uint8_t left = (x >= bytes_per_pixel) ? pixels[x - bytes_per_pixel] : 0;
                    uint8_t up = (y > 0) ? image_data[(y - 1) * stride + 1 + x] : 0;
                    pixels[x] = (pixels[x] + ((left + up) / 2)) & 0xFF;
                }
                break;
                
            case PNG_FILTER_PAETH:
                for (uint32_t x = 0; x < row_bytes; x++) {
                    uint8_t left = (x >= bytes_per_pixel) ? pixels[x - bytes_per_pixel] : 0;
                    uint8_t up = (y > 0) ? image_data[(y - 1) * stride + 1 + x] : 0;
                    uint8_t up_left = (y > 0 && x >= bytes_per_pixel) ?
                                     image_data[(y - 1) * stride + 1 + x - bytes_per_pixel] : 0;
                    pixels[x] = (pixels[x] + paeth_predictor(left, up, up_left)) & 0xFF;
                }
                break;

            default:
                png_debug("[PNG] Unknown filter type %d at row %u\n", filter_type, y);
                break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * DEFLATE (RFC 1951) decoder.
 *
 * Canonical Huffman decoding in the style of zlib's contrib/puff: a table of
 * code counts per length plus symbols sorted by code, decoded one bit at a
 * time with at most 15 steps per symbol.  Fixed and dynamic blocks share the
 * same decoder.
 * ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint32_t bitbuf;
    uint32_t bitcnt;
    bool     overrun;
} inflate_in_t;

static inline uint32_t in_bits(inflate_in_t *s, uint32_t need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->size) {
            s->overrun = true;
            return 0;
        }
        val |= (uint32_t)s->data[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1u << need) - 1);
}

#define HUFF_MAXBITS 15

typedef struct {
    uint16_t count[HUFF_MAXBITS + 1];
    uint16_t symbol[288];
} huff_t;

/* Returns 0 for a complete code, >0 for an incomplete one, <0 if the
 * lengths over-subscribe the code space. */
static int huff_build(huff_t *h, const uint8_t *lengths, int n) {
    uint16_t offs[HUFF_MAXBITS + 1];

    memset(h->count, 0, sizeof(h->count));
    for (int sym = 0; sym < n; sym++)
        h->count[lengths[sym]]++;
    if (h->count[0] == n)
        return 0;

    int left = 1;
    for (int len = 1; len <= HUFF_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }

    offs[1] = 0;
    for (int len = 1; len < HUFF_MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++)
        if (lengths[sym] != 0)
            h->symbol[offs[lengths[sym]]++] = (uint16_t)sym;
    return left;
}

static int huff_decode(inflate_in_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= HUFF_MAXBITS; len++) {
        code |= (int)in_bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->overrun)
            return -1;
    }
    return -1;
}

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Decode one Huffman-coded block into out[*pos..cap). */
static int inflate_codes(inflate_in_t *s, uint8_t *out, uint32_t *pos,
                         uint32_t cap, const huff_t *lencode,
                         const huff_t *distcode) {
    uint32_t p = *pos;
    for (;;) {
        int sym = huff_decode(s, lencode);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (p >= cap)
                return -1;
            out[p++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256)
            break;

        sym -= 257;
        if (sym >= 29)
            return -1;
        uint32_t len = length_base[sym] + in_bits(s, length_extra[sym]);

        int dsym = huff_decode(s, distcode);
        if (dsym < 0 || dsym >= 30)
            return -1;
        uint32_t dist = distance_base[dsym] + in_bits(s, distance_extra[dsym]);

        if (s->overrun || dist > p || len > cap - p)
            return -1;
        const uint8_t *from = out + p - dist;
        uint8_t *to = out + p;
        for (uint32_t i = 0; i < len; i++)
            to[i] = from[i];
        p += len;
    }
    *pos = p;
    return 0;
}

static int inflate_stored(inflate_in_t *s, uint8_t *out, uint32_t *pos,
                          uint32_t cap) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->pos + 4 > s->size)
        return -1;
    uint32_t len  = s->data[s->pos] | ((uint32_t)s->data[s->pos + 1] << 8);
    uint32_t nlen = s->data[s->pos + 2] | ((uint32_t)s->data[s->pos + 3] << 8);
    s->pos += 4;
    if (len != (~nlen & 0xFFFF))
        return -1;
    if (s->pos + len > s->size || len > cap - *pos)
        return -1;
    memcpy(out + *pos, s->data + s->pos, len);
    s->pos += len;
    *pos += len;
    return 0;
}

static huff_t fixed_len, fixed_dist;
static bool fixed_built = false;

static void inflate_build_fixed(void) {
    if (fixed_built)
        return;
    uint8_t lengths[288];
    int sym = 0;
    for (; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < 288; sym++) lengths[sym] = 8;
    huff_build(&fixed_len, lengths, 288);
    for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
    huff_build(&fixed_dist, lengths, 30);
    fixed_built = true;
}

static int inflate_dynamic(inflate_in_t *s, uint8_t *out, uint32_t *pos,
                           uint32_t cap) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t lengths[286 + 30];
    huff_t lencode, distcode;

    uint32_t nlen  = in_bits(s, 5) + 257;
    uint32_t ndist = in_bits(s, 5) + 1;
    uint32_t ncode = in_bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return -1;

    uint32_t index;
    for (index = 0; index < ncode; index++)
        lengths[order[index]] = (uint8_t)in_bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (huff_build(&lencode, lengths, 19) != 0)
        return -1;

    index = 0;
    while (index < nlen + ndist) {
        int sym = huff_decode(s, &lencode);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[index++] = (uint8_t)sym;
            continue;
        }
        uint8_t len = 0;
        uint32_t rep;
        if (sym == 16) {
            if (index == 0)
                return -1;
            len = lengths[index - 1];
            rep = 3 + in_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + in_bits(s, 3);
        } else {
            rep = 11 + in_bits(s, 7);
        }
        if (index + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[index++] = len;
    }
    if (s->overrun || lengths[256] == 0)
        return -1;

    int err = huff_build(&lencode, lengths, (int)nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return -1;
    err = huff_build(&distcode, lengths + nlen, (int)ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return -1;

    return inflate_codes(s, out, pos, cap, &lencode, &distcode);
}

/* Hard ceiling on the inflate output buffer when the exact size is unknown
 * (interlaced / non-8-bit images): prevents a tiny crafted IDAT stream from
 * OOM-ing the kernel via the old `compressed_size * 16` heuristic. */
#define PNG_MAX_DECOMPRESS_SIZE (32u * 1024u * 1024u)

// DEFLATE decompression of a zlib stream.
// expected_size: exact output size from IHDR when known, 0 if unknown.
int png_decompress_data(const uint8_t *compressed_data, uint32_t compressed_size,
                       uint8_t **decompressed_data, uint32_t *decompressed_size,
                       uint32_t expected_size) {
    if (!compressed_data || !decompressed_data || !decompressed_size || compressed_size == 0) {
        png_debug("[PNG] Invalid decompression parameters\n");
        return -1;
    }
    if (compressed_size < 6) {
        png_debug("[PNG] Data too small for zlib header\n");
        return -1;
    }

    // Skip the 2-byte zlib header; the trailing 4 bytes are the Adler-32.
    inflate_in_t s = {
        .data = compressed_data + 2,
        .size = compressed_size - 6,
    };

    // Allocate output buffer: exact size from IHDR when known, otherwise a
    // capped estimate (overflow-safe) so hostile input can't exhaust memory.
    uint32_t cap;
    if (expected_size != 0) {
        cap = expected_size;
    } else {
        uint64_t estimate = (uint64_t)compressed_size * 16;
        if (estimate > PNG_MAX_DECOMPRESS_SIZE) estimate = PNG_MAX_DECOMPRESS_SIZE;
        cap = (uint32_t)estimate;
    }
    uint8_t *out = kalloc(cap);
    if (!out) {
        png_debug("[PNG] Failed to allocate decompression buffer (%u bytes)\n", cap);
        return -1;
    }
    *decompressed_data = out;

    inflate_build_fixed();

    uint32_t pos = 0;
    int err = 0;
    bool last = false;
    while (!last && err == 0) {
        last = in_bits(&s, 1) != 0;
        uint32_t type = in_bits(&s, 2);
        if (s.overrun) {
            err = -1;
            break;
        }
        switch (type) {
        case 0:  err = inflate_stored(&s, out, &pos, cap); break;
        case 1:  err = inflate_codes(&s, out, &pos, cap, &fixed_len, &fixed_dist); break;
        case 2:  err = inflate_dynamic(&s, out, &pos, cap); break;
        default: err = -1; break;
        }
    }

    *decompressed_size = pos;
    png_debug("[PNG] Decompressed %u bytes from %u bytes\n", pos, compressed_size);
    if (err != 0) {
        png_debug("[PNG] Inflate failed (partial output discarded for PNG)\n");
        return -1;
    }
    return 0;
}

/* Non-interlaced 8-bit: exact DEFLATE output size (filter byte + packed pixels per row). */
static int png_expected_raw_inflate_size(uint32_t w, uint32_t h, uint8_t bit_depth,
                                         uint8_t color_type, uint8_t interlace,
                                         uint32_t *out) {
    if (interlace != 0 || bit_depth != 8)
        return -1;
    uint32_t bpp;
    switch (color_type) {
        case 0: bpp = 1; break;
        case 2: bpp = 3; break;
        case 3: bpp = 1; break;
        case 4: bpp = 2; break;
        case 6: bpp = 4; break;
        default: return -1;
    }
    uint64_t row = 1u + (uint64_t)w * (uint64_t)bpp;
    uint64_t tot = row * (uint64_t)h;
    if (tot > 0xFFFFFFFFu)
        return -1;
    *out = (uint32_t)tot;
    return 0;
}

// Convert image data to RGBA pixels
static void png_convert_to_rgba(png_image_t *image, const uint8_t *raw_data) {
    if (!image || !raw_data) {
        png_debug("[PNG] Invalid parameters: image=%p, raw_data=%p\n", image, raw_data);
        return;
    }

        uint32_t pixel_count = image->width * image->height;
    png_debug("[PNG] Allocating pixel buffer: %u pixels (%u bytes)\n", pixel_count, pixel_count * sizeof(uint32_t));
    image->pixels = kalloc(pixel_count * sizeof(uint32_t));

    if (!image->pixels) {
        png_debug("[PNG] Failed to allocate pixel buffer (%u bytes)\n", pixel_count * sizeof(uint32_t));
        return;
    }
    png_debug("[PNG] Successfully allocated pixel buffer\n");
    
    // Calculate bytes per pixel
    uint32_t bytes_per_pixel;
    switch (image->color_type) {
        case PNG_COLOR_RGB:
            bytes_per_pixel = 3;
            break;
        case PNG_COLOR_RGB_ALPHA:
            bytes_per_pixel = 4;
            break;
        case PNG_COLOR_GRAYSCALE:
            bytes_per_pixel = 1;
            break;
        case PNG_COLOR_GRAYSCALE_ALPHA:
            bytes_per_pixel = 2;
            break;
        default:
            bytes_per_pixel = 3; // Default to RGB
    }
    
    uint32_t row_bytes = image->width * bytes_per_pixel;
    uint32_t stride = row_bytes + 1; // +1 for filter byte
    
    png_debug("[PNG] Converting pixels: %dx%d, %d bytes/pixel, stride=%d\n", 
           image->width, image->height, bytes_per_pixel, stride);
    
    // Process each row
    for (uint32_t y = 0; y < image->height; y++) {
        // Check bounds to prevent page fault
        uint64_t row_offset = (uint64_t)y * stride;
        if (row_offset + stride > 1024 * 1024 * 10) { // Reasonable max size limit
            png_debug("[PNG] Row offset too large: %lu\n", row_offset);
            return;
        }

        const uint8_t *row = raw_data + row_offset;
        uint8_t filter_type = row[0];
        const uint8_t *pixels = row + 1; // Skip filter byte
        
        // Simple filter handling - just skip filter for now
        // Disabled filter warnings to reduce debug output
        // if (filter_type != PNG_FILTER_NONE) {
        //     png_debug("[PNG] Warning: Ignoring filter type %d on row %d\n", filter_type, y);
        // }
        
        for (uint32_t x = 0; x < image->width; x++) {
            uint32_t pixel_idx = y * image->width + x;

            // Bounds check for pixel array
            if (pixel_idx >= pixel_count) {
                png_debug("[PNG] Pixel index out of bounds: %u >= %u\n", pixel_idx, pixel_count);
                return;
            }

            uint32_t rgba = 0xFF000000; // Default full alpha
            
            switch (image->color_type) {
                case PNG_COLOR_RGB:
                    if (x * 3 + 2 < row_bytes) {
                        rgba = (0xFF << 24) |                    // Alpha
                               (pixels[x * 3] << 16) |          // Red
                               (pixels[x * 3 + 1] << 8) |       // Green
                               (pixels[x * 3 + 2]);             // Blue
                    }
                    break;
                    
                case PNG_COLOR_RGB_ALPHA:
                    if (x * 4 + 3 < row_bytes) {
                        rgba = (pixels[x * 4 + 3] << 24) |      // Alpha
                               (pixels[x * 4] << 16) |          // Red
                               (pixels[x * 4 + 1] << 8) |       // Green
                               (pixels[x * 4 + 2]);             // Blue
                    }
                    break;
                    
                case PNG_COLOR_GRAYSCALE:
                    if (x < row_bytes) {
                        uint8_t gray = pixels[x];
                        rgba = (0xFF << 24) |                    // Alpha
                               (gray << 16) |                   // Red
                               (gray << 8) |                    // Green
                               (gray);                          // Blue
                    }
                    break;
                    
                case PNG_COLOR_GRAYSCALE_ALPHA:
                    if (x * 2 + 1 < row_bytes) {
                        uint8_t gray = pixels[x * 2];
                        uint8_t alpha = pixels[x * 2 + 1];
                        rgba = (alpha << 24) |                   // Alpha
                               (gray << 16) |                   // Red
                               (gray << 8) |                    // Green
                               (gray);                          // Blue
                    }
                    break;
            }
            
            image->pixels[pixel_idx] = rgba;
        }
    }
    
    png_debug("[PNG] Pixel conversion complete\n");
}

// Decode PNG image
png_image_t* png_decode(const uint8_t *data, uint32_t size) {
    if (!png_validate_signature(data, size)) {
        png_debug("[PNG] Invalid PNG signature\n");
        return NULL;
    }
    
    png_image_t *image = kalloc(sizeof(png_image_t));
    if (!image) {
        return NULL;
    }
    
    memset(image, 0, sizeof(png_image_t));
    
    const uint8_t *ptr = data + PNG_SIGNATURE_SIZE;
    const uint8_t *end = data + size;
    
    uint8_t *combined_idat = NULL;
    uint32_t total_idat_size = 0;
    
    // Parse chunks
    while (ptr < end - 8) {
        uint32_t chunk_length = png_read_be32(ptr);
        ptr += 4;
        
        uint32_t chunk_type = png_read_be32(ptr);
        ptr += 4;
        
        if (ptr + chunk_length + 4 > end) {
            break; // Invalid chunk
        }
        
        switch (chunk_type) {
            case PNG_IHDR:
                if (chunk_length >= 13) {
                    image->width = png_read_be32(ptr);
                    image->height = png_read_be32(ptr + 4);
                    image->bit_depth = ptr[8];
                    image->color_type = ptr[9];
                    image->compression_method = ptr[10];
                    image->filter_method = ptr[11];
                    image->interlace_method = ptr[12];

                    png_debug("[PNG] IHDR data: W=%u H=%u depth=%d color=%d compression=%d filter=%d interlace=%d\n",
                           image->width, image->height, image->bit_depth, image->color_type,
                           image->compression_method, image->filter_method, image->interlace_method);

#if PNG_VERBOSE
                    png_debug("[PNG] IHDR raw bytes: ");
                    for (int i = 0; i < 13; i++) {
                        printf("%02x ", ptr[i]);
                    }
                    printf("\n");
#endif

                    // Validate color type
                    if (image->color_type > 6 || (image->color_type != 0 && image->color_type != 2 &&
                        image->color_type != 3 && image->color_type != 4 && image->color_type != 6)) {
                        png_debug("[PNG] Invalid color type %d, defaulting to RGB\n", image->color_type);
                        image->color_type = PNG_COLOR_RGB; // Default to RGB
                    }

                    // Validate image dimensions
                    if (image->width == 0 || image->height == 0 || 
                        image->width > 4096 || image->height > 4096) {
                        png_debug("[PNG] Invalid image dimensions\n");
                        if (combined_idat) kfree(combined_idat);
                        kfree(image);
                        return NULL;
                    }
                }
                break;
                
            case PNG_IDAT:
                // Validate chunk length
                if (chunk_length == 0 || chunk_length > 1024*1024) {
                    png_debug("[PNG] Invalid IDAT chunk length: %u\n", chunk_length);
                    break;
                }

                // Cap total accumulated IDAT: per-chunk cap alone lets a
                // crafted file with many chunks exhaust kernel memory.
                if (total_idat_size + chunk_length > PNG_MAX_DECOMPRESS_SIZE) {
                    png_debug("[PNG] Total IDAT size exceeds cap, rejecting\n");
                    if (combined_idat) kfree(combined_idat);
                    kfree(image);
                    return NULL;
                }

                // Combine all IDAT chunks
                if (!combined_idat) {
                    combined_idat = kalloc(chunk_length);
                    if (combined_idat) {
                        memcpy(combined_idat, ptr, chunk_length);
                        total_idat_size = chunk_length;
                    } else {
                        png_debug("[PNG] Failed to allocate IDAT buffer\n");
                    }
                } else {
                    // Prevent integer overflow
                    if (total_idat_size + chunk_length < total_idat_size) {
                        png_debug("[PNG] IDAT size overflow\n");
                        break;
                    }
                    
                    uint8_t *new_idat = kalloc(total_idat_size + chunk_length);
                    if (new_idat) {
                        memcpy(new_idat, combined_idat, total_idat_size);
                        memcpy(new_idat + total_idat_size, ptr, chunk_length);
                        kfree(combined_idat);
                        combined_idat = new_idat;
                        total_idat_size += chunk_length;
                    } else {
                        png_debug("[PNG] Failed to reallocate IDAT buffer\n");
                    }
                }
                break;
                
            case PNG_IEND:
                // End of image
                goto parse_complete;
        }
        
        ptr += chunk_length + 4; // Skip chunk data and CRC
    }
    
parse_complete:
    if (!combined_idat || image->width == 0 || image->height == 0) {
        png_debug("[PNG] Invalid image data\n");
        if (combined_idat) kfree(combined_idat);
        kfree(image);
        return NULL;
    }
    
    // Compute the exact inflate output size from IHDR (when supported) so the
    // decompress buffer is sized from image metadata, not attacker-controlled
    // compressed size.
    uint32_t expect_sz = 0;
    bool have_expect =
        (png_expected_raw_inflate_size(image->width, image->height, image->bit_depth,
                                       image->color_type, image->interlace_method,
                                       &expect_sz) == 0);

    // Decompress image data
    uint8_t *decompressed_data;
    uint32_t decompressed_size;

    if (png_decompress_data(combined_idat, total_idat_size,
                           &decompressed_data, &decompressed_size,
                           have_expect ? expect_sz : 0) != 0) {
        png_debug("[PNG] Decompression failed\n");
        kfree(combined_idat);
        kfree(image);
        return NULL;
    }

    if (have_expect) {
        if (decompressed_size != expect_sz) {
            png_debug("[PNG] Inflate size wrong: got %u, expected %u\n",
                   decompressed_size, expect_sz);
            kfree(decompressed_data);
            kfree(combined_idat);
            kfree(image);
            return NULL;
        }
    }

    kfree(combined_idat);
    
    // Apply filters (but simplified for now)
    png_debug("[PNG] Applying PNG filters...\n");
    png_apply_filters(decompressed_data, image->width, image->height,
                     image->color_type, image->bit_depth);
    
    // Convert to RGBA
    png_debug("[PNG] Converting to RGBA format...\n");
    png_convert_to_rgba(image, decompressed_data);
    
    kfree(decompressed_data);
    
    if (!image->pixels) {
        png_debug("[PNG] Failed to convert pixels\n");
        kfree(image);
        return NULL;
    }
    
    image->valid = true;
    png_debug("[PNG] Successfully decoded %dx%d image\n", image->width, image->height);
    return image;
}

// Free PNG image
void png_free(png_image_t *image) {
    if (image) {
        if (image->pixels) {
            kfree(image->pixels);
        }
        kfree(image);
    }
}
