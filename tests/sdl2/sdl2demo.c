/*
 * sdl2demo - SDL2 test program for inDevOS (static musl, SDL2 with the
 * inDevOS /dev/wm video driver).
 *
 *   sdl2demo [seconds]           window on the desktop: animated scene drawn
 *                                with the software renderer, a streaming
 *                                texture, a worker thread and an SDL timer;
 *                                every key/mouse/window event is printed to
 *                                stdout and listed in the window.  Escape or
 *                                the close button quits; optional auto-exit.
 *   sdl2demo --selftest [--driver=NAME]
 *                                headless checks for ostest: init, window,
 *                                renderer, pixel read-back, texture, threads,
 *                                mutex/cond, timer, delay, events, quit.
 *                                Prints SDL2_SELFTEST_OK and exits 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "SDL.h"
#include "SDL_test_font.h"

#define W0 640
#define H0 400
#define LOGN 12

static char evlog[LOGN][96];
static int logn;
static void logev(const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("sdl2demo: %s\n", b);
    fflush(stdout);
    if (logn == LOGN) {
        memmove(evlog[0], evlog[1], sizeof(evlog[0]) * (LOGN - 1));
        logn--;
    }
    SDL_strlcpy(evlog[logn++], b, sizeof(evlog[0]));
}

/* ---- worker thread + timer ---- */
static SDL_atomic_t work_count, timer_ticks, stop_worker;
static int SDLCALL worker(void *arg)
{
    volatile double acc = 0;
    (void)arg;
    while (!SDL_AtomicGet(&stop_worker)) {
        for (int i = 1; i < 20000; i++) acc += 1.0 / i;
        SDL_AtomicAdd(&work_count, 1);
        SDL_Delay(5);
    }
    return 42;
}
static Uint32 SDLCALL tick(Uint32 interval, void *param)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_USEREVENT;
    e.user.code = SDL_AtomicAdd(&timer_ticks, 1) + 1;
    (void)param;
    SDL_PushEvent(&e);
    return interval;
}

/* ---- interactive demo ---- */
static void fill_plasma(SDL_Texture *tex, int tw, int th, Uint32 t)
{
    void *pix;
    int pitch;
    if (SDL_LockTexture(tex, NULL, &pix, &pitch) != 0) return;
    for (int y = 0; y < th; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)pix + y * pitch);
        for (int x = 0; x < tw; x++) {
            int r = (x * 4 + (int)(t / 8)) & 255;
            int g = (y * 4 + (int)(t / 12)) & 255;
            int b = ((x + y) * 2 + (int)(t / 5)) & 255;
            row[x] = 0xFF000000u | (Uint32)(r / 2) << 16 | (Uint32)(g / 2) << 8 | (Uint32)(b / 2 + 40);
        }
    }
    SDL_UnlockTexture(tex);
}

static int demo(int limit_s)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        printf("sdl2demo: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    printf("sdl2demo: SDL %d.%d.%d, video driver '%s'\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver());
    SDL_Window *win = SDL_CreateWindow("SDL2 on inDevOS", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       W0, H0, SDL_WINDOW_RESIZABLE);
    if (!win) { printf("sdl2demo: CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { printf("sdl2demo: CreateRenderer: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(ren, &ri);
    printf("sdl2demo: renderer '%s'\n", ri.name);
    const int TW = 64, TH = 40;
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, TW, TH);
    SDL_StartTextInput();

    SDL_Thread *th = SDL_CreateThread(worker, "worker", NULL);
    SDL_TimerID tid = SDL_AddTimer(1000, tick, NULL);
    Uint32 t0 = SDL_GetTicks(), last_fps_t = t0;
    int frames = 0, fps = 0, running = 1, mx = -1, my = -1, w = W0, h = H0;
    int marks[32][3], nmarks = 0;
    char lastkey[32] = "-", lasttext[32] = "";

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: logev("quit"); running = 0; break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                logev("key %s scancode %d (%s) sym 0x%x mod 0x%x%s", e.type == SDL_KEYDOWN ? "down" : "up",
                      e.key.keysym.scancode, SDL_GetScancodeName(e.key.keysym.scancode),
                      e.key.keysym.sym, e.key.keysym.mod, e.key.repeat ? " repeat" : "");
                if (e.type == SDL_KEYDOWN) {
                    SDL_strlcpy(lastkey, SDL_GetKeyName(e.key.keysym.sym), sizeof(lastkey));
                    if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                }
                break;
            case SDL_TEXTINPUT:
                logev("text '%s'", e.text.text);
                if (strlen(lasttext) + strlen(e.text.text) >= sizeof(lasttext) - 1) lasttext[0] = 0;
                strcat(lasttext, e.text.text);
                break;
            case SDL_MOUSEMOTION:
                mx = e.motion.x; my = e.motion.y;
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                logev("button %d %s at %d,%d clicks %d", e.button.button,
                      e.type == SDL_MOUSEBUTTONDOWN ? "down" : "up", e.button.x, e.button.y, e.button.clicks);
                if (e.type == SDL_MOUSEBUTTONDOWN) {
                    if (nmarks == 32) { memmove(marks[0], marks[1], sizeof(marks[0]) * 31); nmarks--; }
                    marks[nmarks][0] = e.button.x; marks[nmarks][1] = e.button.y; marks[nmarks][2] = e.button.button;
                    nmarks++;
                }
                break;
            case SDL_MOUSEWHEEL:
                logev("wheel %d,%d", e.wheel.x, e.wheel.y);
                break;
            case SDL_WINDOWEVENT:
                switch (e.window.event) {
                case SDL_WINDOWEVENT_RESIZED:
                    logev("window resized %dx%d", e.window.data1, e.window.data2); break;
                case SDL_WINDOWEVENT_FOCUS_GAINED: logev("focus gained"); break;
                case SDL_WINDOWEVENT_FOCUS_LOST: logev("focus lost"); break;
                case SDL_WINDOWEVENT_ENTER: logev("mouse enter"); break;
                case SDL_WINDOWEVENT_LEAVE: logev("mouse leave"); break;
                case SDL_WINDOWEVENT_CLOSE: logev("close request"); break;
                default: break;
                }
                break;
            case SDL_USEREVENT:
                printf("sdl2demo: timer tick %d, fps %d, worker iterations %d\n", e.user.code, fps,
                       SDL_AtomicGet(&work_count));
                fflush(stdout);
                break;
            default: break;
            }
        }
        Uint32 now = SDL_GetTicks();
        if (limit_s && now - t0 > (Uint32)limit_s * 1000) { logev("time limit"); running = 0; }
        SDL_GetRendererOutputSize(ren, &w, &h);

        /* background: plasma texture stretched over the window */
        if (tex) { fill_plasma(tex, TW, TH, now); SDL_RenderCopy(ren, tex, NULL, NULL); }
        /* bouncing squares */
        for (int i = 0; i < 6; i++) {
            float ph = now / 1000.0f * (0.6f + i * 0.17f) + i;
            int bx = (int)((w - 40) * (0.5f + 0.5f * sinf(ph * 1.3f)));
            int by = (int)((h - 40) * (0.5f + 0.5f * cosf(ph)));
            SDL_SetRenderDrawColor(ren, (Uint8)(80 + i * 30), (Uint8)(200 - i * 25), (Uint8)(120 + i * 20), 255);
            SDL_Rect r = { bx, by, 40, 40 };
            SDL_RenderFillRect(ren, &r);
        }
        /* rotating spokes */
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        for (int i = 0; i < 12; i++) {
            float a = now / 900.0f + i * (float)M_PI / 6;
            SDL_RenderDrawLine(ren, w / 2, h / 2, w / 2 + (int)(cosf(a) * 90), h / 2 + (int)(sinf(a) * 90));
        }
        /* click marks */
        for (int i = 0; i < nmarks; i++) {
            SDL_Rect o = { marks[i][0] - 6, marks[i][1] - 6, 12, 12 }, in = { marks[i][0] - 4, marks[i][1] - 4, 8, 8 };
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255); SDL_RenderFillRect(ren, &o);
            SDL_SetRenderDrawColor(ren, marks[i][2] == 1 ? 40 : 230, marks[i][2] == 2 ? 200 : 60, marks[i][2] == 3 ? 240 : 60, 255);
            SDL_RenderFillRect(ren, &in);
        }
        /* crosshair at the pointer */
        if (mx >= 0) {
            SDL_SetRenderDrawColor(ren, 255, 255, 0, 255);
            SDL_RenderDrawLine(ren, mx - 10, my, mx + 10, my);
            SDL_RenderDrawLine(ren, mx, my - 10, mx, my + 10);
        }
        /* text panel */
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
        SDL_Rect panel = { 6, 6, 330, 32 + LOGN * 10 };
        SDL_RenderFillRect(ren, &panel);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        char line[96];
        SDL_SetRenderDrawColor(ren, 120, 255, 160, 255);
        SDL_snprintf(line, sizeof(line), "SDL2 %s  %dx%d  %d fps  t%d w%d", SDL_GetCurrentVideoDriver(), w, h, fps,
                     SDL_AtomicGet(&timer_ticks), SDL_AtomicGet(&work_count));
        SDLTest_DrawString(ren, 12, 12, line);
        SDL_snprintf(line, sizeof(line), "key %s  text \"%s\"  mouse %d,%d", lastkey, lasttext, mx, my);
        SDLTest_DrawString(ren, 12, 24, line);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        for (int i = 0; i < logn; i++) SDLTest_DrawString(ren, 12, 38 + i * 10, evlog[i]);
        SDL_RenderPresent(ren);

        frames++;
        if (now - last_fps_t >= 1000) { fps = frames * 1000 / (int)(now - last_fps_t); frames = 0; last_fps_t = now; }
        SDL_Delay(15);
    }
    SDL_RemoveTimer(tid);
    SDL_AtomicSet(&stop_worker, 1);
    int rc = -1;
    SDL_WaitThread(th, &rc);
    printf("sdl2demo: worker returned %d after %d iterations, %d timer ticks\n", rc, SDL_AtomicGet(&work_count),
           SDL_AtomicGet(&timer_ticks));
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("sdl2demo: exit\n");
    return 0;
}

/* ---- headless self test ---- */
#define T(c, ...) do { if (!(c)) { printf("SDL2_SELFTEST_FAIL "); printf(__VA_ARGS__); printf(" (SDL: %s)\n", SDL_GetError()); return 1; } } while (0)

static SDL_mutex *mx;
static SDL_cond *cv;
static int shared;
static int SDLCALL st_thread(void *arg)
{
    int v = *(int *)arg;
    SDL_LockMutex(mx);
    shared = v * 2;
    SDL_CondSignal(cv);
    SDL_UnlockMutex(mx);
    return v + 1;
}
static Uint32 SDLCALL st_timer(Uint32 iv, void *p)
{
    SDL_AtomicAdd((SDL_atomic_t *)p, 1);
    return iv;
}

static int selftest(const char *driver)
{
    if (driver) SDL_SetHint(SDL_HINT_VIDEODRIVER, driver);
    T(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) == 0, "SDL_Init");
    const char *drv = SDL_GetCurrentVideoDriver();
    T(drv != NULL, "no video driver");
    T(!driver || !strcmp(driver, drv), "driver '%s' (want '%s')", drv, driver);
    SDL_DisplayMode dm;
    T(SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0, "desktop mode");

    SDL_Window *win = SDL_CreateWindow("sdl2 selftest", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 160, 120, 0);
    T(win != NULL, "CreateWindow");
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    T(ww == 160 && wh == 120, "window size %dx%d", ww, wh);
    SDL_SetWindowTitle(win, "sdl2 selftest 2");

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    T(ren != NULL, "CreateRenderer");
    SDL_SetRenderDrawColor(ren, 10, 20, 30, 255);
    T(SDL_RenderClear(ren) == 0, "RenderClear");
    SDL_SetRenderDrawColor(ren, 200, 100, 50, 255);
    SDL_Rect r = { 20, 10, 30, 40 };
    T(SDL_RenderFillRect(ren, &r) == 0, "FillRect");
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 8, 8);
    T(tex != NULL, "CreateTexture");
    Uint32 tp[64];
    for (int i = 0; i < 64; i++) tp[i] = 0xFF00FF00u;
    T(SDL_UpdateTexture(tex, NULL, tp, 32) == 0, "UpdateTexture");
    SDL_Rect dr = { 100, 60, 16, 16 };
    T(SDL_RenderCopy(ren, tex, NULL, &dr) == 0, "RenderCopy");
    Uint32 px[3] = { 0, 0, 0 };
    SDL_Rect p0 = { 5, 5, 1, 1 }, p1 = { 30, 30, 1, 1 }, p2 = { 108, 68, 1, 1 };
    T(SDL_RenderReadPixels(ren, &p0, SDL_PIXELFORMAT_ARGB8888, &px[0], 4) == 0, "ReadPixels");
    SDL_RenderReadPixels(ren, &p1, SDL_PIXELFORMAT_ARGB8888, &px[1], 4);
    SDL_RenderReadPixels(ren, &p2, SDL_PIXELFORMAT_ARGB8888, &px[2], 4);
    T((px[0] & 0xFFFFFF) == 0x0A141E, "clear pixel %08x", px[0]);
    T((px[1] & 0xFFFFFF) == 0xC86432, "rect pixel %08x", px[1]);
    T((px[2] & 0xFFFFFF) == 0x00FF00, "texture pixel %08x", px[2]);
    SDL_RenderPresent(ren);

    /* threads, mutex, cond */
    mx = SDL_CreateMutex();
    cv = SDL_CreateCond();
    T(mx && cv, "mutex/cond");
    int arg = 21, ret = 0;
    SDL_LockMutex(mx);
    SDL_Thread *th = SDL_CreateThread(st_thread, "st", &arg);
    T(th != NULL, "CreateThread");
    while (shared == 0) T(SDL_CondWaitTimeout(cv, mx, 3000) == 0 || shared, "cond wait timed out");
    SDL_UnlockMutex(mx);
    SDL_WaitThread(th, &ret);
    T(shared == 42 && ret == 22, "thread shared=%d ret=%d", shared, ret);

    /* timer + delay */
    SDL_atomic_t fired;
    SDL_AtomicSet(&fired, 0);
    Uint64 c0 = SDL_GetPerformanceCounter();
    Uint32 t0 = SDL_GetTicks();
    SDL_TimerID tid = SDL_AddTimer(20, st_timer, &fired);
    T(tid != 0, "AddTimer");
    SDL_Delay(150);
    Uint32 el = SDL_GetTicks() - t0;
    double pel = (double)(SDL_GetPerformanceCounter() - c0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    SDL_RemoveTimer(tid);
    int nf = SDL_AtomicGet(&fired);
    T(el >= 140 && el < 2000, "Delay(150) took %u ms", el);
    T(pel >= 140 && pel < 2000, "perf counter says %.1f ms", pel);
    T(nf >= 2, "timer fired %d times in %u ms", nf, el);

    /* events */
    SDL_Event e;
    while (SDL_PollEvent(&e)) { }
    SDL_zero(e);
    e.type = SDL_USEREVENT;
    e.user.code = 1234;
    T(SDL_PushEvent(&e) == 1, "PushEvent");
    int got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        SDL_Event g;
        if (SDL_WaitEventTimeout(&g, 20) && g.type == SDL_USEREVENT && g.user.code == 1234) got = 1;
    }
    T(got, "user event not received");

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_DestroyCond(cv);
    SDL_DestroyMutex(mx);
    SDL_Quit();
    printf("SDL2_SELFTEST_OK driver=%s mode=%dx%d delay150=%ums timer=%d thread=%d\n", drv, dm.w, dm.h, el, nf, ret);
    return 0;
}

int main(int argc, char **argv)
{
    const char *driver = NULL;
    int self = 0, limit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) self = 1;
        else if (!strncmp(argv[i], "--driver=", 9)) driver = argv[i] + 9;
        else limit = atoi(argv[i]);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (self) return selftest(driver);
    if (driver) SDL_SetHint(SDL_HINT_VIDEODRIVER, driver);
    return demo(limit);
}
