#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "herbengineC3D.c"

/* The engine writes 0x00RRGGBB into pixels[].
   Canvas ImageData expects bytes [R, G, B, A] in memory order.
   On little-endian that means each uint32 must be 0xFFBBGGRR.
   We keep two buffers: engine writes to engine_pixels, we convert
   to rgba_pixels before handing to the canvas.                    */
static uint32_t *engine_pixels = NULL;
static uint32_t *rgba_pixels   = NULL;

static void convert_to_rgba(void) {
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        uint32_t p = engine_pixels[i];
        uint8_t r = (p >> 16) & 0xFF;
        uint8_t g = (p >>  8) & 0xFF;
        uint8_t b =  p        & 0xFF;
        /* canvas uint32 (LE) = [R, G, B, A] = 0xFF_BB_GG_RR */
        rgba_pixels[i] = (0xFF000000u) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }
}

/* ---- mouse ---- */
static EM_BOOL on_mousemove(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    if (holding_mouse) {
        mouse.x = WIDTH  / 2 + e->movementX;
        mouse.y = HEIGHT / 2 + e->movementY;
    }
    return EM_TRUE;
}

static EM_BOOL on_mousedown(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    if (e->button == 0) mouse_left_click  = 1;
    if (e->button == 2) mouse_right_click = 1;
    emscripten_request_pointerlock("#canvas", 0);
    holding_mouse = 1;
    return EM_TRUE;
}

static EM_BOOL on_mouseup(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    if (e->button == 0) mouse_left_click  = 0;
    if (e->button == 2) mouse_right_click = 0;
    return EM_TRUE;
}

/* ---- keyboard ---- */
static int browser_key_to_index(const char *key) {
    if (strcmp(key, "KeyW")         == 0) return 'w';
    if (strcmp(key, "KeyA")         == 0) return 'a';
    if (strcmp(key, "KeyS")         == 0) return 's';
    if (strcmp(key, "KeyD")         == 0) return 'd';
    if (strcmp(key, "Space")        == 0) return ' ';
    if (strcmp(key, "ShiftLeft")    == 0 ||
        strcmp(key, "ShiftRight")   == 0) return 128;
    if (strcmp(key, "ControlLeft")  == 0 ||
        strcmp(key, "ControlRight") == 0) return 129;
    if (strcmp(key, "Escape")       == 0) return 130;
    if (strcmp(key, "Digit1")       == 0) return '1';
    if (strcmp(key, "Digit2")       == 0) return '2';
    if (strcmp(key, "Digit3")       == 0) return '3';
    if (strcmp(key, "Digit4")       == 0) return '4';
    if (strcmp(key, "Digit5")       == 0) return '5';
    if (strcmp(key, "Digit6")       == 0) return '6';
    if (strcmp(key, "Digit7")       == 0) return '7';
    if (strcmp(key, "Digit8")       == 0) return '8';
    if (strcmp(key, "Digit9")       == 0) return '9';
    return -1;
}

static EM_BOOL on_keydown(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type; (void)ud;
    int idx = browser_key_to_index(e->code);
    if (idx >= 0) keys[idx] = 1;
    return EM_TRUE;
}

static EM_BOOL on_keyup(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type; (void)ud;
    int idx = browser_key_to_index(e->code);
    if (idx >= 0) keys[idx] = 0;
    return EM_TRUE;
}

/* called from JS when pointer lock changes */
EMSCRIPTEN_KEEPALIVE
void set_holding_mouse(int val) {
    holding_mouse = val;
}

/* ---- main loop ---- */
static void main_loop(void) {
    update();
    convert_to_rgba();

    mouse.x = WIDTH  / 2;
    mouse.y = HEIGHT / 2;

    EM_ASM({
        var buf   = new Uint8ClampedArray(HEAPU8.buffer, $0, $1 * $2 * 4);
        var idata = new ImageData(buf, $1, $2);
        Module._ctx.putImageData(idata, 0, 0);
    }, rgba_pixels, WIDTH, HEIGHT);
}

int main(void) {

    EM_ASM({
        var canvas = document.getElementById('canvas');
        if (!canvas) {
            canvas = document.createElement('canvas');
            canvas.id = 'canvas';
            document.body.appendChild(canvas);
        }
        canvas.width  = $0;
        canvas.height = $1;
        canvas.style.display = 'block';
        canvas.style.margin  = '0 auto';
        document.body.style.background = '#000';
        document.body.style.margin     = '0';
        canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });
        document.addEventListener('pointerlockchange', function() {
            var locked = (document.pointerLockElement === canvas);
            Module.ccall('set_holding_mouse', null, ['number'], [locked ? 1 : 0]);
        });
        Module._ctx = canvas.getContext('2d');
    }, WIDTH, HEIGHT);

    engine_pixels = (uint32_t *)malloc(WIDTH * HEIGHT * 4);
    rgba_pixels   = (uint32_t *)malloc(WIDTH * HEIGHT * 4);
    memset(engine_pixels, 0, WIDTH * HEIGHT * 4);
    memset(rgba_pixels,   0, WIDTH * HEIGHT * 4);

    /* point the engine at our buffer */
    pixels = engine_pixels;

    /* key indices */
    w       = 'w';
    a       = 'a';
    s       = 's';
    d       = 'd';
    space   = ' ';
    shift   = 128;
    control = 129;
    escape  = 130;
    one   = '1';  two   = '2';  three = '3';
    four  = '4';  five  = '5';  six   = '6';
    seven = '7';  eight = '8';  nine  = '9';

    emscripten_set_mousemove_callback("#canvas", NULL, 1, on_mousemove);
    emscripten_set_mousedown_callback("#canvas", NULL, 1, on_mousedown);
    emscripten_set_mouseup_callback  ("#canvas", NULL, 1, on_mouseup);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_keydown);
    emscripten_set_keyup_callback  (EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_keyup);

    holding_mouse = 0;
    mouse_defined = 1;

    init_stuff();

    emscripten_set_main_loop(main_loop, TARGET_FPS, 1);

    cleanup();
    free(engine_pixels);
    free(rgba_pixels);
    return 0;
}