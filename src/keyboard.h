#pragma once
#include "types.h"
#include "registers.h"
#include "pic.h"
#include "idt.h"

// Keyboard buffer and state are now internal and protected by spinlocks
// Use keyboard_get_buffer() and keyboard_get_execute() to access safely
bool keyboard_get_execute(void);
char* keyboard_get_buffer(void);
uint32_t keyboard_get_buffer_length(void);
void keyboard_clear_execute(void);
void keyboard_clear_buffer(void);
int keyboard_consume_char(void);  // Safely consume and return first character (-1 if empty)

// Safe echo processing functions (call from main thread)
void keyboard_process_echo(void);
bool keyboard_has_echo_pending(void);

// Deferred SDL key events (call from main thread to drain ISR queue)
void keyboard_process_sdl_events(void);
bool keyboard_has_sdl_pending(void);

// Tab completion support
bool keyboard_get_tab_requested(void);
void keyboard_clear_tab_requested(void);

// Scrollback support
bool keyboard_get_pgup_pressed(void);
bool keyboard_get_pgdn_pressed(void);
void keyboard_clear_pgup(void);
void keyboard_clear_pgdn(void);

// Arrow key support for command history
bool keyboard_get_arrow_up(void);
bool keyboard_get_arrow_down(void);
void keyboard_clear_arrow_up(void);
void keyboard_clear_arrow_down(void);

// Additional arrow keys
bool keyboard_get_arrow_left(void);
bool keyboard_get_arrow_right(void);
void keyboard_clear_arrow_left(void);
void keyboard_clear_arrow_right(void);

// Escape key
bool keyboard_get_escape(void);
void keyboard_clear_escape(void);

// Home / End
bool keyboard_get_home(void);
void keyboard_clear_home(void);
bool keyboard_get_end(void);
void keyboard_clear_end(void);

// Function keys
bool keyboard_get_f1(void);
void keyboard_clear_f1(void);
bool keyboard_get_f5(void);
void keyboard_clear_f5(void);

// Virtual console switch (Ctrl+Q)
bool keyboard_get_vconsole_switch(void);
void keyboard_clear_vconsole_switch(void);

// Raw mode: suppress echo and normal buffer, just queue chars for consume
void keyboard_set_raw_mode(bool enable);
bool keyboard_get_raw_mode(void);

void keyboard_set_buffer(const char *str, uint32_t len);
uint32_t keyboard_get_cursor(void);
void keyboard_set_cursor(uint32_t pos);
bool keyboard_take_dirty(void);
void keyboard_delete_range(uint32_t start, uint32_t end);

bool keyboard_get_alt(void);
bool keyboard_get_super(void);

bool keyboard_get_kill_line(void);
void keyboard_clear_kill_line(void);
bool keyboard_get_kill_word(void);
void keyboard_clear_kill_word(void);
void keyboard_inject_string(const char *s, int len);

void keyboard_feed_char(char ch);

/* Reset the active VC for interactive shell input after a foreground app exits. */
void keyboard_restore_shell_input(void);

// Keyboard layout support
void keyboard_set_layout(int layout_id);
int  keyboard_get_layout(void);
const char* keyboard_get_layout_name(void);
#define KB_LAYOUT_US    0
#define KB_LAYOUT_UK    1
#define KB_LAYOUT_DE    2
#define KB_LAYOUT_FR    3
#define KB_LAYOUT_MAX   4

void keyboard_init();
void keyboard_poll(void);
char ps2_translate2ascii(uint8_t code, bool shift, bool caps);
void keyboard_handler();