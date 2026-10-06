/**
 * @file render.c
 * @brief Software 2D renderer used by the desktop compositor and SDL
 */
#include "render.h"
#include "string.h"
#include <stddef.h>

extern const uint8_t font_16x8[128][16];

static inline int r_imin(int a, int b) { return a < b ? a : b; }
static inline int r_imax(int a, int b) { return a > b ? a : b; }
static inline int r_iclamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void r_target_init(r_target_t *t, uint32_t *pixels, int w, int h, int pitch)
{
    t->pixels = pixels;
    t->w = w;
    t->h = h;
    t->pitch = pitch;
    r_clip_reset(t);
}

void r_target_from_bytes(r_target_t *t, void *pixels, int w, int h, int pitch_bytes)
{
    r_target_init(t, (uint32_t *)pixels, w, h, pitch_bytes / 4);
}

void r_clip(r_target_t *t, int x0, int y0, int x1, int y1)
{
    t->clip_x0 = r_imax(0, x0);
    t->clip_y0 = r_imax(0, y0);
    t->clip_x1 = r_imin(t->w, x1);
    t->clip_y1 = r_imin(t->h, y1);
    if (t->clip_x1 < t->clip_x0) t->clip_x1 = t->clip_x0;
    if (t->clip_y1 < t->clip_y0) t->clip_y1 = t->clip_y0;
}

void r_clip_reset(r_target_t *t)
{
    t->clip_x0 = 0;
    t->clip_y0 = 0;
    t->clip_x1 = t->w;
    t->clip_y1 = t->h;
}

void r_clip_intersect(r_target_t *t, int x0, int y0, int x1, int y1)
{
    r_clip(t,
           r_imax(t->clip_x0, x0),
           r_imax(t->clip_y0, y0),
           r_imin(t->clip_x1, x1),
           r_imin(t->clip_y1, y1));
}

uint32_t r_over(uint32_t dst, uint32_t src)
{
    uint32_t sa = src >> 24;
    if (sa == 0) return dst;
    if (sa >= 255) return src | 0xFF000000u;
    uint32_t da = 255u - sa;
    uint32_t sr = (src >> 16) & 0xFFu;
    uint32_t sg = (src >> 8) & 0xFFu;
    uint32_t sb = src & 0xFFu;
    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8) & 0xFFu;
    uint32_t db = dst & 0xFFu;
    uint32_t r = (sr * sa + dr * da) >> 8;
    uint32_t g = (sg * sa + dg * da) >> 8;
    uint32_t b = (sb * sa + db * da) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline bool r_clip_point(const r_target_t *t, int x, int y)
{
    return x >= t->clip_x0 && x < t->clip_x1 &&
           y >= t->clip_y0 && y < t->clip_y1 &&
           t->pixels;
}

static bool r_clip_rect(const r_target_t *t, int *x, int *y, int *w, int *h)
{
    if (!t->pixels) return false;
    int x0 = *x;
    int y0 = *y;
    int x1 = x0 + *w;
    int y1 = y0 + *h;
    if (x0 < t->clip_x0) x0 = t->clip_x0;
    if (y0 < t->clip_y0) y0 = t->clip_y0;
    if (x1 > t->clip_x1) x1 = t->clip_x1;
    if (y1 > t->clip_y1) y1 = t->clip_y1;
    if (x0 >= x1 || y0 >= y1) return false;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

void r_put(r_target_t *t, int x, int y, uint32_t c)
{
    if (!r_clip_point(t, x, y)) return;
    t->pixels[(uint32_t)y * (uint32_t)t->pitch + (uint32_t)x] = c;
}

void r_blend(r_target_t *t, int x, int y, uint32_t c)
{
    if (!r_clip_point(t, x, y)) return;
    uint32_t *p = &t->pixels[(uint32_t)y * (uint32_t)t->pitch + (uint32_t)x];
    *p = r_over(*p, c);
}

void r_fill(r_target_t *t, int x, int y, int w, int h, uint32_t c)
{
    if (!r_clip_rect(t, &x, &y, &w, &h)) return;
    uint32_t *first = t->pixels + (uint32_t)y * (uint32_t)t->pitch + (uint32_t)x;
    for (int i = 0; i < w; i++)
        first[i] = c;
    for (int yy = 1; yy < h; yy++) {
        memcpy(t->pixels + (uint32_t)(y + yy) * (uint32_t)t->pitch + (uint32_t)x,
               first, (size_t)w * 4u);
    }
}

void r_fill_blend(r_target_t *t, int x, int y, int w, int h, uint32_t c)
{
    if ((c >> 24) == 0) return;
    if ((c >> 24) >= 255) {
        r_fill(t, x, y, w, h, c);
        return;
    }
    if (!r_clip_rect(t, &x, &y, &w, &h)) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = t->pixels + (uint32_t)(y + yy) * (uint32_t)t->pitch + (uint32_t)x;
        for (int xx = 0; xx < w; xx++)
            row[xx] = r_over(row[xx], c);
    }
}

void r_hline(r_target_t *t, int x, int y, int w, uint32_t c)
{
    r_fill(t, x, y, w, 1, c);
}

void r_vline(r_target_t *t, int x, int y, int h, uint32_t c)
{
    r_fill(t, x, y, 1, h, c);
}

void r_rect(r_target_t *t, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    r_hline(t, x, y, w, c);
    r_hline(t, x, y + h - 1, w, c);
    r_vline(t, x, y, h, c);
    r_vline(t, x + w - 1, y, h, c);
}

void r_line(r_target_t *t, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int x = x0, y = y0;
    for (;;) {
        r_put(t, x, y, c);
        if (x == x1 && y == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx)  { err += dx; y += sy; }
    }
}

static uint32_t r_lerp_rgb(uint32_t c0, uint32_t c1, int num, int den)
{
    if (den <= 0) return c0;
    if (num <= 0) return c0;
    if (num >= den) return c1;
    int r0 = (int)((c0 >> 16) & 0xFFu);
    int g0 = (int)((c0 >> 8) & 0xFFu);
    int b0 = (int)(c0 & 0xFFu);
    int r1 = (int)((c1 >> 16) & 0xFFu);
    int g1 = (int)((c1 >> 8) & 0xFFu);
    int b1 = (int)(c1 & 0xFFu);
    int r = r0 + (r1 - r0) * num / den;
    int g = g0 + (g1 - g0) * num / den;
    int b = b0 + (b1 - b0) * num / den;
    uint32_t a = (c0 >> 24);
    return (a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void r_vgrad(r_target_t *t, int x, int y, int w, int h, uint32_t c0, uint32_t c1)
{
    if (h <= 0 || w <= 0) return;
    int x0 = x, y0 = y, ww = w, hh = h;
    if (!r_clip_rect(t, &x0, &y0, &ww, &hh)) return;
    for (int yy = 0; yy < hh; yy++) {
        int src_y = (y0 + yy) - y;
        uint32_t c = r_lerp_rgb(c0, c1, src_y, h - 1);
        r_hline(t, x0, y0 + yy, ww, c);
    }
}

void r_hgrad(r_target_t *t, int x, int y, int w, int h, uint32_t c0, uint32_t c1)
{
    if (h <= 0 || w <= 0) return;
    int x0 = x, y0 = y, ww = w, hh = h;
    if (!r_clip_rect(t, &x0, &y0, &ww, &hh)) return;
    for (int xx = 0; xx < ww; xx++) {
        int src_x = (x0 + xx) - x;
        uint32_t c = r_lerp_rgb(c0, c1, src_x, w - 1);
        r_vline(t, x0 + xx, y0, hh, c);
    }
}

static int r_span_half(int rad, int dy)
{
    int remain = rad * rad - dy * dy;
    if (remain < 0) return -1;
    int lo = 0, hi = rad;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (mid * mid <= remain) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static void r_circle_spans(r_target_t *t, int cx, int cy, int rad, uint32_t c, bool blend)
{
    if (rad < 0 || !t->pixels) return;
    if (rad == 0) {
        if (blend) r_blend(t, cx, cy, c);
        else r_put(t, cx, cy, c);
        return;
    }
    int dy0 = -rad;
    int dy1 = rad;
    if (cy + dy0 < t->clip_y0) dy0 = t->clip_y0 - cy;
    if (cy + dy1 >= t->clip_y1) dy1 = t->clip_y1 - cy - 1;
    if (dy0 > dy1) return;
    for (int dy = dy0; dy <= dy1; dy++) {
        int hx = r_span_half(rad, dy);
        if (hx < 0) continue;
        int y = cy + dy;
        int x = cx - hx;
        int w = hx * 2 + 1;
        if (blend) r_fill_blend(t, x, y, w, 1, c);
        else r_fill(t, x, y, w, 1, c);
    }
}

void r_circle(r_target_t *t, int cx, int cy, int rad, uint32_t c)
{
    r_circle_spans(t, cx, cy, rad, c, false);
}

void r_circle_blend(r_target_t *t, int cx, int cy, int rad, uint32_t c)
{
    r_circle_spans(t, cx, cy, rad, c, true);
}

static int r_clamp_rad(int w, int h, int rad)
{
    if (rad < 0) rad = 0;
    if (w < 2 || h < 2) return 0;
    int maxr = r_imin(w, h) / 2;
    return r_imin(rad, maxr);
}

static void r_rrect_body(r_target_t *t, int x, int y, int w, int h, int rad,
                         uint32_t c, bool blend)
{
    rad = r_clamp_rad(w, h, rad);
    if (rad == 0) {
        if (blend) r_fill_blend(t, x, y, w, h, c);
        else r_fill(t, x, y, w, h, c);
        return;
    }
    void (*fill)(r_target_t *, int, int, int, int, uint32_t) =
        blend ? r_fill_blend : r_fill;
    fill(t, x + rad, y, w - 2 * rad, h, c);
    fill(t, x, y + rad, rad, h - 2 * rad, c);
    fill(t, x + w - rad, y + rad, rad, h - 2 * rad, c);
    for (int dy = 0; dy < rad; dy++) {
        int hx = r_span_half(rad, rad - dy);
        if (hx < 0) continue;
        int span = hx + 1;
        fill(t, x + rad - hx, y + dy, span, 1, c);
        fill(t, x + w - rad, y + dy, span, 1, c);
        fill(t, x + rad - hx, y + h - 1 - dy, span, 1, c);
        fill(t, x + w - rad, y + h - 1 - dy, span, 1, c);
    }
}

void r_rrect(r_target_t *t, int x, int y, int w, int h, int rad, uint32_t c)
{
    r_rrect_body(t, x, y, w, h, rad, c, false);
}

void r_rrect_blend(r_target_t *t, int x, int y, int w, int h, int rad, uint32_t c)
{
    r_rrect_body(t, x, y, w, h, rad, c, true);
}

void r_rrect_outline(r_target_t *t, int x, int y, int w, int h, int rad, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    rad = r_clamp_rad(w, h, rad);
    if (rad == 0) {
        r_rect(t, x, y, w, h, c);
        return;
    }
    r_hline(t, x + rad, y, w - 2 * rad, c);
    r_hline(t, x + rad, y + h - 1, w - 2 * rad, c);
    r_vline(t, x, y + rad, h - 2 * rad, c);
    r_vline(t, x + w - 1, y + rad, h - 2 * rad, c);
    for (int dy = 0; dy < rad; dy++) {
        int hx = r_span_half(rad, rad - dy);
        if (hx < 0) continue;
        r_put(t, x + rad - hx, y + dy, c);
        r_put(t, x + w - 1 - (rad - hx), y + dy, c);
        r_put(t, x + rad - hx, y + h - 1 - dy, c);
        r_put(t, x + w - 1 - (rad - hx), y + h - 1 - dy, c);
    }
}

void r_shadow(r_target_t *t, int x, int y, int w, int h, int rad, int spread)
{
    /* One opaque offset plate — the old multi-layer blend was O(spread * w * h). */
    if (spread <= 0 || w <= 0 || h <= 0) return;
    int o = spread > 4 ? 4 : spread;
    (void)rad;
    r_fill(t, x + 3, y + o, w, h, 0xFF07080C);
}

void r_blit(r_target_t *t, int dx, int dy,
            const uint32_t *src, int sw, int sh, int spitch)
{
    if (!t->pixels || !src || sw <= 0 || sh <= 0) return;
    int x0 = 0, y0 = 0, x1 = sw, y1 = sh;
    if (dx + x0 < t->clip_x0) x0 = t->clip_x0 - dx;
    if (dy + y0 < t->clip_y0) y0 = t->clip_y0 - dy;
    if (dx + x1 > t->clip_x1) x1 = t->clip_x1 - dx;
    if (dy + y1 > t->clip_y1) y1 = t->clip_y1 - dy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 >= x1 || y0 >= y1) return;
    size_t row_bytes = (size_t)(x1 - x0) * 4u;
    for (int row = y0; row < y1; row++) {
        memcpy(t->pixels + (uint32_t)(dy + row) * (uint32_t)t->pitch + (uint32_t)(dx + x0),
               src + (uint32_t)row * (uint32_t)spitch + (uint32_t)x0,
               row_bytes);
    }
}

void r_blit_keyed(r_target_t *t, int dx, int dy,
                  const uint32_t *src, int sw, int sh, int spitch)
{
    if (!t->pixels || !src || sw <= 0 || sh <= 0) return;
    int x0 = 0, y0 = 0, x1 = sw, y1 = sh;
    if (dx + x0 < t->clip_x0) x0 = t->clip_x0 - dx;
    if (dy + y0 < t->clip_y0) y0 = t->clip_y0 - dy;
    if (dx + x1 > t->clip_x1) x1 = t->clip_x1 - dx;
    if (dy + y1 > t->clip_y1) y1 = t->clip_y1 - dy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 >= x1 || y0 >= y1) return;
    for (int row = y0; row < y1; row++) {
        const uint32_t *s = src + (uint32_t)row * (uint32_t)spitch;
        uint32_t *d = t->pixels + (uint32_t)(dy + row) * (uint32_t)t->pitch + (uint32_t)dx;
        int col = x0;
        while (col < x1) {
            if ((s[col] & 0xFF000000u) == 0) { col++; continue; }
            int run = col;
            while (col < x1 && (s[col] & 0xFF000000u) != 0) col++;
            memcpy(d + run, s + run, (size_t)(col - run) * 4u);
        }
    }
}

static inline int r_glyph_on(const uint8_t *g, int col, int row)
{
    if (col < 0 || col > 7 || row < 0 || row > 15) return 0;
    return (g[row] >> (7 - col)) & 1;
}

void r_char(r_target_t *t, int x, int y, char c, uint32_t color)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font_16x8[(int)c - 32];
    uint32_t rgb = color & 0x00FFFFFFu;
    uint32_t solid = rgb | 0xFF000000u;
    for (int row = 0; row < 16; row++) {
        int col = 0;
        while (col < 8) {
            if (!r_glyph_on(glyph, col, row)) {
                int n = r_glyph_on(glyph, col - 1, row)
                      + r_glyph_on(glyph, col + 1, row)
                      + r_glyph_on(glyph, col, row - 1)
                      + r_glyph_on(glyph, col, row + 1);
                if (n)
                    r_blend(t, x + col, y + row, rgb | ((uint32_t)(48 * n) << 24));
                col++;
                continue;
            }
            int run = col + 1;
            while (run < 8 && r_glyph_on(glyph, run, row))
                run++;
            int mid0 = col + 1;
            int mid1 = run - 1;
            if (mid1 > mid0)
                r_fill(t, x + mid0, y + row, mid1 - mid0, 1, solid);
            r_blend(t, x + col, y + row, rgb | 0xE0000000u);
            if (run - 1 != col)
                r_blend(t, x + run - 1, y + row, rgb | 0xE0000000u);
            col = run;
        }
    }
}

void r_text(r_target_t *t, int x, int y, const char *s, uint32_t color)
{
    if (!s) return;
    while (*s) {
        r_char(t, x, y, *s, color);
        x += 8;
        s++;
    }
}

int r_text_width(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n * 8;
}

void r_copy_rect(const r_target_t *src, void *dst, uint32_t dst_pitch_bytes,
                 int x0, int y0, int x1, int y1)
{
    if (!src || !src->pixels || !dst) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > src->w) x1 = src->w;
    if (y1 > src->h) y1 = src->h;
    if (x0 >= x1 || y0 >= y1) return;
    uint8_t *d = (uint8_t *)dst;
    uint32_t bytes = (uint32_t)(x1 - x0) * 4u;
    for (int row = y0; row < y1; row++) {
        memcpy(d + (uint32_t)row * dst_pitch_bytes + (uint32_t)x0 * 4u,
               src->pixels + (uint32_t)row * (uint32_t)src->pitch + (uint32_t)x0,
               bytes);
    }
}
