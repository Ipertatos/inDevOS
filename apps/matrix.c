/*
 * matrix - falling green "digital rain" in its own desktop window.
 *
 * This replaces the in-kernel src/usoft/matrix.c, which blitted characters
 * into the legacy 80x25 VGA text buffer - a surface the framebuffer console
 * does not use any more, so the effect was invisible.  As a real program it
 * keeps a small cell grid, rasterises it into an ARGB buffer and hands the
 * finished frame to the compositor with a single SDL_CopyPixels syscall.
 */
#include "syscalls.h"

#define WIN_W     640
#define WIN_H     400
#define CELL_W    8
#define CELL_H    16
#define COLS      (WIN_W / CELL_W)   /* 80 */
#define ROWS      (WIN_H / CELL_H)   /* 25 */
#define TRAIL     8                  /* cells of fading tail behind the head */
#define HEAD_ROW  (ROWS - 1)

/* SDL event subset we react to (see syscalls.h / sdl_events.c). */
#define SDL_QUIT_EVENT     0x100
#define SDL_KEYDOWN_EVENT  0x300

typedef struct { uint32_t type; uint32_t timestamp; } sdl_quit_event_t;
typedef struct {
    uint32_t type;
    uint32_t timestamp;
    uint32_t window_id;
    uint8_t  state;
    uint8_t  repeat;
    uint8_t  padding2;
    uint8_t  padding3;
    int32_t  keysym_scancode;
    int32_t  keysym_sym;
    int32_t  keysym_mod;
} sdl_key_event_t;
typedef union { uint32_t type; sdl_quit_event_t quit; sdl_key_event_t key; } sdl_event_t;

#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_TIMER 0x00000001u

static uint32_t pixels[WIN_W * WIN_H];
static uint8_t  level[ROWS][COLS];      /* 0 = head, higher = older, 0xFF empty */
static int      head[COLS];
static int      drop_len[COLS];

static uint32_t rng_state = 0x2468ACE1u;

static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint32_t shade(int age) {
    /* Bright near-white head, fading to dark green. */
    static const uint32_t ramp[TRAIL + 1] = {
        0x00E0FFE8u, 0x0090FFA0u, 0x0058E070u, 0x0038B050u,
        0x00288838u, 0x001C6828u, 0x00144C1Cu, 0x000E3814u, 0x000A2A10u
    };
    if (age < 0) age = 0;
    if (age > TRAIL) age = TRAIL;
    return ramp[age];
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* SDL_INIT_TIMER is needed for SDL_GetTicks() to report a real timebase. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        printf("matrix: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    void *window = SDL_CreateWindow("matrix", 40, 40, WIN_W, WIN_H, 0);
    if (!window) {
        printf("matrix: cannot create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    void *renderer = SDL_CreateRenderer(window, -1, 1 /* SDL_RENDERER_SOFTWARE */);
    if (!renderer) {
        printf("matrix: cannot create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            level[r][c] = 0xFF;
    for (int c = 0; c < COLS; c++) {
        head[c] = -1;
        drop_len[c] = 0;
    }

    /* The Enter that launched the program is still in the keyboard buffer -
     * drop it (or any other pending keys) so the first frame is not also the
     * last, exactly as the old in-kernel version did. */
    kb_clear();

    int running = 1;
    uint32_t frames = 0;
    uint32_t last_log = SDL_GetTicks();
    while (running) {
        sdl_event_t ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT_EVENT) running = 0;
            else if (ev.type == SDL_KEYDOWN_EVENT) running = 0;
        }
        /* Any console input (including Ctrl+C) also stops it. */
        char kbuf[8];
        if (kb_read(kbuf, sizeof(kbuf)) > 0 || terminate_requested())
            running = 0;
        if (!running) break;

        /* Age the whole grid, wiping cells that finished their fall. */
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                uint8_t lv = level[r][c];
                if (lv == 0xFF) continue;
                if (lv >= TRAIL) level[r][c] = 0xFF;
                else level[r][c] = (uint8_t)(lv + 1);
            }
        }

        /* Advance / spawn drops and place their bright head. */
        for (int c = 0; c < COLS; c++) {
            if (head[c] < 0) {
                /* ~5% chance per column per frame: enough drops alive at once
                 * to read as continuous rain rather than isolated specks. */
                if ((rng() & 0xFFu) < 12) {
                    head[c] = 0;
                    drop_len[c] = TRAIL + (int)(rng() % (uint32_t)(ROWS - TRAIL));
                }
                continue;
            }
            if (head[c] < ROWS) level[head[c]][c] = 0;
            head[c]++;
            if (head[c] - TRAIL > drop_len[c]) head[c] = -1;
        }

        /* Rasterise the grid: black background, one block per live cell with
         * the brightness of the head fading down the tail. */
        uint32_t *p = pixels;
        for (int r = 0; r < ROWS; r++) {
            for (int y = 0; y < CELL_H; y++) {
                for (int c = 0; c < COLS; c++) {
                    uint8_t lv = level[r][c];
                    uint32_t color = (lv == 0xFF) ? 0x00000000u : shade(lv);
                    uint32_t *row = p + (r * CELL_H + y) * WIN_W + c * CELL_W;
                    row[0] = color;
                    row[1] = color;
                    row[2] = color;
                    row[3] = color;
                    row[4] = color;
                    row[5] = color;
                }
            }
        }

        SDL_CopyPixelsFromUser(renderer, pixels, WIN_W, WIN_H);
        SDL_RenderPresent(renderer);
        SDL_Delay(40);

        frames++;
        if (frames % 20 == 0) {
            uint32_t now = SDL_GetTicks();
            uint32_t elapsed = now - last_log;
            printf("matrix: %u frames (%u ms, %u fps)\n", frames, elapsed,
                   elapsed ? (20u * 1000u) / elapsed : 0u);
            last_log = now;
        }
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("matrix: stopped\n");
    return 0;
}
