/*
 * wmdemo - /dev/wm demo client (static musl).
 * Opens a 480x320 window, draws a gradient, drops a marker where you click
 * (left = green, right = red, middle = blue), shifts the gradient hue on each
 * key press, redraws at the new size when the frame is resized, and exits on
 * the title-bar close button (or Escape / 'q').
 *   wmdemo [seconds]   optional auto-exit after N seconds (tests)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wm.h"

#define W0 480
#define H0 320

static int hue;
static int marks[64][3], nmarks;

static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void draw(wm_window_t *win, int focused) {
    uint32_t *px = wm_window_pixels(win);
    int W = win->w, H = win->h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int r = (x * 255 / (W - 1) + hue) & 255;
            int g = (y * 255 / (H - 1) + hue / 2) & 255;
            int b = (255 - x * 255 / (W - 1) + hue * 3) & 255;
            px[(size_t)y * win->stride_px + x] = wm_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    /* translucent band at the bottom (premultiplied, 50% black) */
    wm_fill_rect(win, 0, H - 24, W, 24, wm_premul(0x80000000u));
    /* focus indicator */
    wm_fill_rect(win, 8, H - 18, 12, 12, focused ? wm_rgb(61, 220, 151) : wm_rgb(90, 90, 90));
    /* hue indicator */
    wm_fill_rect(win, 28, H - 18, (hue % 256) * (W - 40) / 255 + 4, 12, wm_rgb(255, 255, 255));
    for (int i = 0; i < nmarks; i++) {
        uint32_t c = marks[i][2] == 1 ? wm_rgb(40, 220, 90) : marks[i][2] == 2 ? wm_rgb(230, 60, 60)
                                                                               : wm_rgb(60, 120, 240);
        wm_fill_rect(win, marks[i][0] - 6, marks[i][1] - 6, 12, 12, wm_rgb(255, 255, 255));
        wm_fill_rect(win, marks[i][0] - 4, marks[i][1] - 4, 8, 8, c);
    }
}

int main(int argc, char **argv) {
    int limit_s = argc > 1 ? atoi(argv[1]) : 0;
    wm_conn_t c;
    int r = wm_connect(&c);
    if (r < 0) { printf("wmdemo: cannot open %s: %s\n", WM_DEVICE_PATH, strerror(-r)); return 1; }
    printf("wmdemo: connected, screen %ux%u, abi %u.%u\n", c.info.screen_w, c.info.screen_h, c.info.version, c.info.minor);
    wm_window_t win;
    r = wm_window_create(&c, &win, W0, H0, "wmdemo", WM_TYPE_NORMAL, WM_FLAG_RESIZABLE);
    if (r < 0) { printf("wmdemo: create failed: %s\n", strerror(-r)); return 1; }
    printf("wmdemo: window %u %dx%d mapped at %p\n", win.id, win.w, win.h, win.map);
    int focused = 1;
    draw(&win, focused);
    wm_window_present(&win, 0, 0, 0, 0, 0);
    uint64_t t0 = now_ms();
    int presents = 1, running = 1;
    while (running) {
        struct wm_event ev;
        int n = wm_next_event(&c, &ev, 100);
        if (n < 0) { printf("wmdemo: event error %s\n", strerror(-n)); break; }
        if (limit_s && now_ms() - t0 > (uint64_t)limit_s * 1000) { printf("wmdemo: time limit\n"); break; }
        if (n == 0) continue;
        int redraw = 0;
        switch (ev.type) {
        case WM_EV_BUTTON_DOWN:
            printf("wmdemo: button %d down at %d,%d clicks %d\n", ev.button, ev.x, ev.y, ev.clicks);
            if (nmarks < 64) { marks[nmarks][0] = ev.x; marks[nmarks][1] = ev.y; marks[nmarks][2] = ev.button; nmarks++; }
            redraw = 1;
            break;
        case WM_EV_WHEEL:
            hue += ev.wheel * 8; redraw = 1; break;
        case WM_EV_KEY_DOWN:
            printf("wmdemo: key %d\n", ev.key);
            if (ev.key == 27 || ev.key == 'q') running = 0;
            hue += 24; redraw = 1;
            break;
        case WM_EV_FOCUS_IN:  focused = 1; redraw = 1; printf("wmdemo: focus in\n"); break;
        case WM_EV_FOCUS_OUT: focused = 0; redraw = 1; printf("wmdemo: focus out\n"); break;
        case WM_EV_RESIZE: {
            int rr = wm_window_resize(&win, (int)ev.w, (int)ev.h);
            printf("wmdemo: resize %ux%u -> %s\n", ev.w, ev.h, rr ? strerror(-rr) : "ok");
            redraw = 1;
            break;
        }
        case WM_EV_CLOSE_REQUEST:
            printf("wmdemo: close request\n"); running = 0; break;
        default: break;
        }
        if (redraw && running) {
            draw(&win, focused);
            wm_window_present(&win, 0, 0, 0, 0, 0);
            presents++;
            char t[64];
            snprintf(t, sizeof(t), "wmdemo %dx%d - %d marks", win.w, win.h, nmarks);
            wm_window_set_title(&win, t);
        }
    }
    printf("wmdemo: exit after %d presents\n", presents);
    wm_window_destroy(&win);
    wm_disconnect(&c);
    return 0;
}
