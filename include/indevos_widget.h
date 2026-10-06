/**
 * Minimal userland widgets for inDevOS desktop apps (inspired by toolkit patterns;
 * implemented independently — not a port of external code).
 *
 * Draw into your window RGBA buffer, then indevos_desktop_composite().
 * Use indevos_desktop_poll_event() for mouse/key; map window-local coords with
 * indevos_desktop_embed_query().
 */
#ifndef INDEVOS_WIDGET_H
#define INDEVOS_WIDGET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int x, y, w, h;
    const char *label;
    bool pressed;
} indevos_button_t;

static inline void indevos_fill_rect(uint32_t *px, uint32_t pitch_px,
                                     int x, int y, int w, int h, uint32_t rgba) {
    if (!px || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t *line = px + (size_t)(y + row) * (size_t)pitch_px + (size_t)x;
        for (int col = 0; col < w; col++)
            line[col] = rgba;
    }
}

static inline bool indevos_button_contains(const indevos_button_t *b, int lx, int ly) {
    return b && lx >= b->x && lx < b->x + b->w && ly >= b->y && ly < b->y + b->h;
}

/** Simple filled button (no text rendering — label is for your own debug). */
static inline void indevos_button_draw(uint32_t *px, uint32_t pitch_px, indevos_button_t *b) {
    if (!b || !px) return;
    uint32_t bg = b->pressed ? 0xFF336644u : 0xFF445566u;
    uint32_t border = 0xFF88AA99u;
    indevos_fill_rect(px, pitch_px, b->x, b->y, b->w, b->h, bg);
    indevos_fill_rect(px, pitch_px, b->x, b->y, b->w, 2, border);
    indevos_fill_rect(px, pitch_px, b->x, b->y + b->h - 2, b->w, 2, border);
    indevos_fill_rect(px, pitch_px, b->x, b->y, 2, b->h, border);
    indevos_fill_rect(px, pitch_px, b->x + b->w - 2, b->y, 2, b->h, border);
}

#endif
