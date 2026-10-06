/**
 * Minimal desktop demo: embed window + RGBA buffer + indevos_widget button + poll events.
 * Build with your userland toolchain (see build_userapp.sh / Makefile patterns).
 */
#include "../include/syscalls.h"
#include "../include/indevos_gui.h"
#include "../include/indevos_widget.h"

static uint32_t pxbuf[640 * 480];

int main(void) {
    indevos_desktop_embed_info_t info;
    if (indevos_desktop_embed_query(&info) != 0 || !info.active)
        return 1;

    int w = info.content_w > 0 ? info.content_w : 640;
    int h = info.content_h > 0 ? info.content_h : 480;
    if (w > 640) w = 640;
    if (h > 480) h = 480;

    indevos_button_t btn = { 40, 40, 120, 36, "OK", false };

    for (;;) {
        for (int i = 0; i < w * h; i++)
            pxbuf[i] = 0xFF202428u;

        indevos_button_draw(pxbuf, (uint32_t)w, &btn);

        indevos_desktop_composite(pxbuf, (uint32_t)w, (uint32_t)h);

        gui_event_t ev;
        while (indevos_desktop_poll_event(&ev) == 1) {
            if (ev.type == GUI_EVENT_MOUSE_DOWN && ev.arg3 == 1) {
                int lx = ev.arg1;
                int ly = ev.arg2;
                if (indevos_button_contains(&btn, lx, ly)) {
                    btn.pressed = true;
                }
            }
            if (ev.type == GUI_EVENT_MOUSE_UP && ev.arg3 == 1) {
                btn.pressed = false;
            }
            if (ev.type == GUI_EVENT_CLOSE)
                return 0;
        }

        timespec_ind_t ts = {0, 10 * 1000000L};
        nanosleep(&ts, NULL);
    }
}
