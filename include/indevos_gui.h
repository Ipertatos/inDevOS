/**
 * inDevOS GUI event ABI (kernel + userland).
 * Inspired by common desktop event models; keep in sync with kernel desktop.c.
 */
#ifndef INDEVOS_GUI_H
#define INDEVOS_GUI_H

#include <stdint.h>

#define GUI_EVENT_NONE        0
#define GUI_EVENT_PAINT       1
#define GUI_EVENT_CLICK       2
#define GUI_EVENT_RIGHT_CLICK 3
#define GUI_EVENT_CLOSE       4
#define GUI_EVENT_KEY         5
#define GUI_EVENT_KEYUP       10
#define GUI_EVENT_MOUSE_DOWN  6
#define GUI_EVENT_MOUSE_UP    7
#define GUI_EVENT_MOUSE_MOVE  8
#define GUI_EVENT_MOUSE_WHEEL 9
#define GUI_EVENT_RESIZE      11

typedef struct {
    int32_t type;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
} gui_event_t;

#define GUI_EVENT_QUEUE_CAP 32

typedef struct {
    gui_event_t q[GUI_EVENT_QUEUE_CAP];
    uint32_t head;
    uint32_t tail;
} gui_event_queue_t;

#endif
