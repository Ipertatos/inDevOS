/*
  inDevOS video driver for SDL2: every SDL window is a /dev/wm window on the
  inDevOS desktop (ABI in wm_abi.h, shared with the kernel).

  Copyright (C) 2026 Georgios Markakis.  zlib license (same as SDL).
*/
#include "../../SDL_internal.h"

#ifndef SDL_indevosvideo_h_
#define SDL_indevosvideo_h_

#include "../SDL_sysvideo.h"

typedef struct SDL_VideoData
{
    int fd;          /* /dev/wm connection (one per process)             */
    int wake[2];     /* self-pipe for SendWakeupEvent                    */
    int screen_w, screen_h;
} SDL_VideoData;

typedef struct SDL_WindowData
{
    Uint32 id;       /* WM window id (0 = none)                          */
    void *map;       /* both surfaces (we draw/present buffer 0)         */
    size_t map_len;
    int stride;      /* bytes per row                                    */
    int w, h;        /* surface size                                     */
    int pending_w, pending_h;   /* WM_EV_RESIZE not yet acknowledged     */
} SDL_WindowData;

extern SDL_Window *INDEVOS_FindWindow(_THIS, Uint32 id);
extern void INDEVOS_PumpEvents(_THIS);
extern int INDEVOS_WaitEventTimeout(_THIS, int timeout);
extern void INDEVOS_SendWakeupEvent(_THIS, SDL_Window *window);
extern void INDEVOS_InitKeyboard(void);

#endif /* SDL_indevosvideo_h_ */

/* vi: set ts=4 sw=4 expandtab: */
