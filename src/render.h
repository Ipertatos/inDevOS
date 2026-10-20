/**
 * @file render.h
 * @brief Software 2D renderer (XRGB8888 / packed 32-bit)
 *
 * Clip-aware fills, blits, rounded rects, gradients, text, and present.
 * Colour values are stored as-is; callers choose the pixel layout
 * (desktop uses 0xAARRGGBB, SDL maps through the surface format).
 */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct r_target {
    uint32_t *pixels;
    int       w;
    int       h;
    int       pitch;          /* pixels per row */
    int       clip_x0, clip_y0, clip_x1, clip_y1;
} r_target_t;

void r_target_init(r_target_t *t, uint32_t *pixels, int w, int h, int pitch);
void r_target_from_bytes(r_target_t *t, void *pixels, int w, int h, int pitch_bytes);
void r_clip(r_target_t *t, int x0, int y0, int x1, int y1);
void r_clip_reset(r_target_t *t);
void r_clip_intersect(r_target_t *t, int x0, int y0, int x1, int y1);

uint32_t r_over(uint32_t dst, uint32_t src);

void r_put(r_target_t *t, int x, int y, uint32_t c);
void r_blend(r_target_t *t, int x, int y, uint32_t c);
void r_fill(r_target_t *t, int x, int y, int w, int h, uint32_t c);
void r_fill_blend(r_target_t *t, int x, int y, int w, int h, uint32_t c);
void r_hline(r_target_t *t, int x, int y, int w, uint32_t c);
void r_vline(r_target_t *t, int x, int y, int h, uint32_t c);
void r_rect(r_target_t *t, int x, int y, int w, int h, uint32_t c);
void r_line(r_target_t *t, int x0, int y0, int x1, int y1, uint32_t c);

void r_vgrad(r_target_t *t, int x, int y, int w, int h, uint32_t c0, uint32_t c1);
void r_hgrad(r_target_t *t, int x, int y, int w, int h, uint32_t c0, uint32_t c1);

void r_circle(r_target_t *t, int cx, int cy, int rad, uint32_t c);
void r_circle_blend(r_target_t *t, int cx, int cy, int rad, uint32_t c);
void r_rrect(r_target_t *t, int x, int y, int w, int h, int rad, uint32_t c);
void r_rrect_blend(r_target_t *t, int x, int y, int w, int h, int rad, uint32_t c);
void r_rrect_outline(r_target_t *t, int x, int y, int w, int h, int rad, uint32_t c);

void r_shadow(r_target_t *t, int x, int y, int w, int h, int rad, int spread);

void r_blit(r_target_t *t, int dx, int dy,
            const uint32_t *src, int sw, int sh, int spitch);
void r_blit_premul(r_target_t *t, int dx, int dy,
                   const uint32_t *src, int sw, int sh, int spitch);
void r_blit_keyed(r_target_t *t, int dx, int dy,
                  const uint32_t *src, int sw, int sh, int spitch);

void r_char(r_target_t *t, int x, int y, char c, uint32_t color);
void r_text(r_target_t *t, int x, int y, const char *s, uint32_t color);
int  r_text_width(const char *s);

/* Copy a rect (x1/y1 exclusive) from target to a packed 32-bit destination. */
void r_copy_rect(const r_target_t *src, void *dst, uint32_t dst_pitch_bytes,
                 int x0, int y0, int x1, int y1);

#endif /* RENDER_H */
