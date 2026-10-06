/*
 * inDevOS port of herb-engine-C-3D (linux-style: framebuffer + input loop).
 * Upstream: https://github.com/42henry/herb-engine-C-3D
 *
 * Build: ./build_userapp.sh herb_engine_indevos.c herb_engine.exe
 * Run:   exec herb_engine.exe
 */
#define __INDEVOS_USERAPP__ 1

/*
 * Internal render resolution = window size. For more FPS on slow hosts, compile
 * with smaller -DWIDTH / -DHEIGHT (build_userapp.sh already uses -O2 for this app).
 */
#ifndef WIDTH
#define WIDTH 1280
#endif
#ifndef HEIGHT
#define HEIGHT 800
#endif

#include "include/herb_indevos_compat.h"

/* Relative mouse motion for mouselook (no pointer warp in inDevOS SDL yet). */
int herb_indevos_mouse_dx;
int herb_indevos_mouse_dy;

#include "third_party/herb-engine-C-3D/herbengineC3D.c"

#define INDEVOS_NO_BUILTIN_MALLOC 1
#define INDEVOS_NO_ERRNO 1
#include "include/syscalls.h"

/* Must match kernel include/SDL.h + sdl_event_types.h (syscall copies sizeof(SDL_Event)). */
#include "include/sdl_event_types.h"

typedef int SDL_Keycode;

#define SDL_QUIT 0x100
#define SDL_KEYDOWN 0x300
#define SDL_KEYUP 0x301
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP 0x402
#define SDL_MOUSEMOTION 0x403

#define SDL_INIT_TIMER 0x00000001
#define SDL_INIT_VIDEO 0x00000020
#define SDL_INIT_EVENTS 0x00000040

#define SDL_WINDOW_SHOWN 0x00000004
#define SDL_RENDERER_SOFTWARE 0x00000001

#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_RIGHT 3

#define SDLK_LSHIFT 0x200
#define SDLK_RSHIFT 0x201
#define SDLK_LCTRL 0x202
#define SDLK_RCTRL 0x203
#define SDLK_w ((SDL_Keycode)'w')
#define SDLK_a ((SDL_Keycode)'a')
#define SDLK_s ((SDL_Keycode)'s')
#define SDLK_d ((SDL_Keycode)'d')
#define SDLK_SPACE ((SDL_Keycode)' ')
#define SDLK_ESCAPE 0x1B
#define SDLK_1 ((SDL_Keycode)'1')
#define SDLK_2 ((SDL_Keycode)'2')
#define SDLK_3 ((SDL_Keycode)'3')
#define SDLK_4 ((SDL_Keycode)'4')
#define SDLK_5 ((SDL_Keycode)'5')
#define SDLK_6 ((SDL_Keycode)'6')
#define SDLK_7 ((SDL_Keycode)'7')
#define SDLK_8 ((SDL_Keycode)'8')
#define SDLK_9 ((SDL_Keycode)'9')

#define HERB_KEY_SHIFT 250
#define HERB_KEY_CTRL 251

static int herb_running = 1;

static uint8_t herb_kb_snapshot[1024];

/* Set by herb_apply_input_snapshot — used when draining SDL events after syscalls */
static int herb_kb_syscall_ok;
static int herb_mouse_syscall_had_rel;

static void herb_apply_key_event(const SDL_Event *e, int is_down) {
    int32_t sym = e->key.sym;
    if (sym >= 0 && sym < 256)
        keys[sym] = is_down ? 1 : 0;
    if (sym == (int32_t)SDLK_LSHIFT || sym == (int32_t)SDLK_RSHIFT)
        keys[HERB_KEY_SHIFT] = is_down ? 1 : 0;
    if (sym == (int32_t)SDLK_LCTRL || sym == (int32_t)SDLK_RCTRL)
        keys[HERB_KEY_CTRL] = is_down ? 1 : 0;
}

static void herb_apply_input_snapshot(void) {
    herb_kb_syscall_ok = 0;
    herb_mouse_syscall_had_rel = 0;

    /* Keyboard and mouse are independent — never skip mouse if keyboard syscall fails. */
    if (indevos_get_keyboard_state(herb_kb_snapshot, sizeof(herb_kb_snapshot)) == 0) {
        herb_kb_syscall_ok = 1;
        int i;
        for (i = 0; i < 256; i++)
            keys[i] = herb_kb_snapshot[i] ? 1 : 0;
        keys[HERB_KEY_SHIFT] = (herb_kb_snapshot[SDLK_LSHIFT] || herb_kb_snapshot[SDLK_RSHIFT]) ? 1 : 0;
        keys[HERB_KEY_CTRL] = (herb_kb_snapshot[SDLK_LCTRL] || herb_kb_snapshot[SDLK_RCTRL]) ? 1 : 0;
    }

    {
        uint64_t m = indevos_get_mouse_rel_accum();
        int32_t mdx = (int32_t)(m >> 32);
        int32_t mdy = (int32_t)(m & 0xffffffffu);
        if (mdx != 0 || mdy != 0)
            herb_mouse_syscall_had_rel = 1;
        herb_indevos_mouse_dx += mdx;
        herb_indevos_mouse_dy += mdy;
    }

    if (herb_kb_syscall_ok) {
        #define ARROW_LOOK_SPEED 5
        #define KB_UP    0x100
        #define KB_DOWN  0x101
        #define KB_LEFT  0x102
        #define KB_RIGHT 0x103
        if (herb_kb_snapshot[KB_LEFT])   herb_indevos_mouse_dx -= ARROW_LOOK_SPEED;
        if (herb_kb_snapshot[KB_RIGHT])  herb_indevos_mouse_dx += ARROW_LOOK_SPEED;
        if (herb_kb_snapshot[KB_UP])     herb_indevos_mouse_dy -= ARROW_LOOK_SPEED;
        if (herb_kb_snapshot[KB_DOWN])   herb_indevos_mouse_dy += ARROW_LOOK_SPEED;
        #undef KB_UP
        #undef KB_DOWN
        #undef KB_LEFT
        #undef KB_RIGHT
        #undef ARROW_LOOK_SPEED
    }

    {
        uint8_t b = (uint8_t)indevos_get_mouse_buttons();
        mouse_left_click = (b & 1) ? 1 : 0;
        mouse_right_click = (b & 2) ? 1 : 0;
    }
}

static void herb_poll_events(void) {
    static SDL_Event e;

    /*
     * Apply kernel input syscalls FIRST, then drain the SDL queue.
     * Previously we drained PollEvent before syscalls — motion/key events were
     * popped and thrown away every frame, and PumpEvents() never ran before
     * the accumulators we actually used, making debugging confusing.
     */
    herb_apply_input_snapshot();

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            herb_running = 0;
            break;
        }
        /* Fallback: if keyboard bitmap syscall failed, drive keys from queued SDL key events */
        if (!herb_kb_syscall_ok) {
            if (e.type == SDL_KEYDOWN)
                herb_apply_key_event(&e, 1);
            else if (e.type == SDL_KEYUP)
                herb_apply_key_event(&e, 0);
        }
        /*
         * Fallback: PS/2 rel accum may be empty at screen edge (position clamped) or if
         * syscall path misbehaves — SDL_MOUSEMOTION carries xrel/yrel from PumpEvents().
         */
        if (!herb_mouse_syscall_had_rel && e.type == SDL_MOUSEMOTION) {
            herb_indevos_mouse_dx += e.motion.xrel;
            herb_indevos_mouse_dy += e.motion.yrel;
        }
    }
}

void _start(void) {
    w = (unsigned char)SDLK_w;
    a = (unsigned char)SDLK_a;
    s = (unsigned char)SDLK_s;
    d = (unsigned char)SDLK_d;
    space = (unsigned char)SDLK_SPACE;
    escape = (unsigned char)SDLK_ESCAPE;
    one = (unsigned char)SDLK_1;
    two = (unsigned char)SDLK_2;
    three = (unsigned char)SDLK_3;
    four = (unsigned char)SDLK_4;
    five = (unsigned char)SDLK_5;
    six = (unsigned char)SDLK_6;
    seven = (unsigned char)SDLK_7;
    eight = (unsigned char)SDLK_8;
    nine = (unsigned char)SDLK_9;
    shift = HERB_KEY_SHIFT;
    control = HERB_KEY_CTRL;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        print("herb_engine: SDL_Init failed\n");
        exit(1);
    }

    void *window = SDL_CreateWindow("Herb Engine (inDevOS)", 0, 0, WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        print("herb_engine: SDL_CreateWindow failed\n");
        SDL_Quit();
        exit(1);
    }

    void *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        print("herb_engine: SDL_CreateRenderer failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        exit(1);
    }

    /* 8-byte aligned so clear_screen can use 64-bit stores. */
    static uint32_t herb_pixel_buf[WIDTH * HEIGHT] __attribute__((aligned(8)));
    pixels = herb_pixel_buf;

    init_stuff();

    while (herb_running) {
        herb_poll_events();
        if (syscall(SYS_GET_TERMINATE_FLAG, 0, 0, 0, 0, 0)) {
            herb_running = 0;
            break;
        }

        update();
        if (SDL_CopyPixelsFromUser(renderer, pixels, (uint32_t)WIDTH, (uint32_t)HEIGHT) != 0) {
            print("herb_engine: SDL_CopyPixelsFromUser failed\n");
            break;
        }
        SDL_RenderPresent(renderer);

       // SDL_Delay(16);
    }

    cleanup();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    exit(0);
}
