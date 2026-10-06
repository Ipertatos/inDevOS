#pragma once
/*
 * tui.h - Text User Interface Menu API for inDevOS
 *
 * Provides menuconfig-style TUI menus with box-drawing borders,
 * color themes, keyboard navigation, and various item types.
 *
 * Usage:
 *   tui_menu_t *menu = tui_menu_create("My Settings", "Configure things");
 *   tui_menu_add(menu, "Option A",           TUI_ITEM_SUBMENU, 1);
 *   tui_menu_add(menu, "Enable feature X",   TUI_ITEM_CHECKBOX, 2);
 *   tui_menu_add(menu, "Disable feature Y",  TUI_ITEM_CHECKBOX, 3);
 *   tui_menu_add(menu, "About",              TUI_ITEM_TEXT, 4);
 *
 *   int result = tui_menu_run(menu);   // blocks until user exits
 *   int tag    = tui_menu_get_selected_tag(menu);
 *   tui_menu_destroy(menu);
 */

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Key codes returned by tui_read_key()
 * ================================================================ */
#define TUI_KEY_NONE      0
#define TUI_KEY_UP        256
#define TUI_KEY_DOWN      257
#define TUI_KEY_LEFT      258
#define TUI_KEY_RIGHT     259
#define TUI_KEY_ENTER     260
#define TUI_KEY_ESCAPE    261
#define TUI_KEY_SPACE     262
#define TUI_KEY_TAB       263
#define TUI_KEY_BACKSPACE 264
#define TUI_KEY_HOME      265
#define TUI_KEY_END       266
#define TUI_KEY_PGUP      267
#define TUI_KEY_PGDN      268
#define TUI_KEY_F1        280
#define TUI_KEY_F5        284

/* ================================================================
 * Menu item types
 * ================================================================ */
#define TUI_ITEM_TEXT      0  /* Plain selectable text item              */
#define TUI_ITEM_SUBMENU   1  /* Opens a submenu (shows " --->" suffix)  */
#define TUI_ITEM_CHECKBOX  2  /* Toggleable  [*] / [ ]                   */
#define TUI_ITEM_RADIO     3  /* Radio group (X) / ( )                   */
#define TUI_ITEM_SEPARATOR 4  /* Non-selectable horizontal line          */
#define TUI_ITEM_LABEL     5  /* Non-selectable informational text       */

/* ================================================================
 * Menu run results
 * ================================================================ */
#define TUI_RESULT_SELECT   0  /* User pressed Enter on an item          */
#define TUI_RESULT_EXIT     1  /* User pressed Escape / Exit             */
#define TUI_RESULT_HELP     2  /* User pressed F1 / Help                 */

/* ================================================================
 * Color theme presets
 * ================================================================ */
#define TUI_THEME_BLUE      0  /* Classic menuconfig blue theme          */
#define TUI_THEME_CYAN      1  /* Cyan/teal variant                      */
#define TUI_THEME_DARK      2  /* Dark/gray theme                        */
#define TUI_THEME_GREEN     3  /* Green hacker theme                     */

/* ================================================================
 * Limits
 * ================================================================ */
#define TUI_MAX_ITEMS       64
#define TUI_MAX_LABEL      128
#define TUI_MAX_HELP       256
#define TUI_MAX_TITLE       80
#define TUI_MAX_BUTTONS      5
#define TUI_MAX_BUTTON_LEN  16

/* ================================================================
 * Structures
 * ================================================================ */

/* A single menu item */
typedef struct tui_item {
    char   label[TUI_MAX_LABEL]; /* Display text                         */
    char   help[TUI_MAX_HELP];   /* Help/description text                */
    int    type;                  /* TUI_ITEM_*                           */
    int    value;                 /* For checkbox/radio: 0=off, 1=on      */
    int    tag;                   /* User-defined identifier              */
    int    group;                 /* Radio group id (items with same group
                                  * are mutually exclusive)               */
    bool   enabled;               /* Greyed out if false                  */
    void  *submenu;               /* Pointer to tui_menu_t for SUBMENU    */
    void  *userdata;              /* User-defined pointer                 */
} tui_item_t;

/* A button in the bottom bar */
typedef struct tui_button {
    char   label[TUI_MAX_BUTTON_LEN];
    int    key;                   /* TUI_KEY_* that activates it          */
} tui_button_t;

/* Color theme */
typedef struct tui_theme {
    uint32_t bg;           /* Screen background (ANSI color code)   */
    uint32_t border_fg;    /* Border foreground                     */
    uint32_t border_bg;    /* Border background                     */
    uint32_t title_fg;     /* Title text foreground                 */
    uint32_t title_bg;     /* Title text background                 */
    uint32_t item_fg;      /* Normal item foreground                */
    uint32_t item_bg;      /* Normal item background                */
    uint32_t sel_fg;       /* Selected item foreground              */
    uint32_t sel_bg;       /* Selected item background              */
    uint32_t disabled_fg;  /* Disabled/label foreground             */
    uint32_t help_fg;      /* Help text foreground                  */
    uint32_t help_bg;      /* Help text background                  */
    uint32_t btn_fg;       /* Button foreground                     */
    uint32_t btn_bg;       /* Button background                     */
    uint32_t btn_sel_fg;   /* Active button foreground              */
    uint32_t btn_sel_bg;   /* Active button background              */
    uint32_t tag_fg;       /* Checkbox/radio marker foreground      */
} tui_theme_t;

/* A complete menu */
typedef struct tui_menu {
    char         title[TUI_MAX_TITLE];
    char         subtitle[TUI_MAX_HELP]; /* Instruction text at top      */
    tui_item_t   items[TUI_MAX_ITEMS];
    int          item_count;
    int          selected;               /* Currently highlighted index  */
    int          scroll_offset;          /* For scrollable menus         */
    int          visible_rows;           /* How many items fit on screen */
    int          result;                 /* Last TUI_RESULT_*            */

    /* Buttons */
    tui_button_t buttons[TUI_MAX_BUTTONS];
    int          button_count;
    int          active_button;          /* Currently focused button     */

    /* Layout (computed during render) */
    int          win_x, win_y;           /* Window top-left (chars)      */
    int          win_w, win_h;           /* Window size (chars)          */

    /* Theme */
    tui_theme_t  theme;
} tui_menu_t;

/* ================================================================
 * API - Menu lifecycle
 * ================================================================ */

/* Create a menu with given title and optional subtitle (help text).
 * Returns NULL on allocation failure. */
tui_menu_t *tui_menu_create(const char *title, const char *subtitle);

/* Free a menu and all its resources. */
void tui_menu_destroy(tui_menu_t *menu);

/* ================================================================
 * API - Adding items
 * ================================================================ */

/* Add an item to the menu. Returns the item index, or -1 on failure. */
int tui_menu_add(tui_menu_t *menu, const char *label, int type, int tag);

/* Add a submenu item. The submenu pointer is stored; caller still owns it. */
int tui_menu_add_submenu(tui_menu_t *menu, const char *label,
                         tui_menu_t *submenu, int tag);

/* Add a separator line. */
int tui_menu_add_separator(tui_menu_t *menu);

/* Set help text for an existing item by index. */
void tui_menu_set_help(tui_menu_t *menu, int index, const char *help);

/* Set the radio group for a RADIO item. */
void tui_menu_set_group(tui_menu_t *menu, int index, int group);

/* Set initial checkbox/radio value. */
void tui_menu_set_value(tui_menu_t *menu, int index, int value);

/* Enable or disable an item. */
void tui_menu_set_enabled(tui_menu_t *menu, int index, bool enabled);

/* ================================================================
 * API - Buttons
 * ================================================================ */

/* Add a button to the bottom bar. */
void tui_menu_add_button(tui_menu_t *menu, const char *label, int key);

/* Add the standard Select / Exit / Help button set. */
void tui_menu_add_default_buttons(tui_menu_t *menu);

/* ================================================================
 * API - Running the menu
 * ================================================================ */

/* Display the menu and block until the user selects/exits.
 * Returns TUI_RESULT_SELECT, TUI_RESULT_EXIT, or TUI_RESULT_HELP. */
int tui_menu_run(tui_menu_t *menu);

/* ================================================================
 * API - Querying results
 * ================================================================ */

/* Get the tag of the currently selected item. */
int tui_menu_get_selected_tag(tui_menu_t *menu);

/* Get the index of the currently selected item. */
int tui_menu_get_selected_index(tui_menu_t *menu);

/* Get checkbox/radio value for the item with the given tag.
 * Returns -1 if tag not found. */
int tui_menu_get_value(tui_menu_t *menu, int tag);

/* Get item by tag. Returns NULL if not found. */
tui_item_t *tui_menu_get_item(tui_menu_t *menu, int tag);

/* ================================================================
 * API - Theming
 * ================================================================ */

/* Apply a built-in theme preset. */
void tui_menu_set_theme(tui_menu_t *menu, int theme_id);

/* Get a writable pointer to the theme for custom tweaks. */
tui_theme_t *tui_menu_get_theme(tui_menu_t *menu);

/* ================================================================
 * API - Convenience dialogs
 * ================================================================ */

/* Show a message box with OK button. Blocks until dismissed. */
void tui_message_box(const char *title, const char *message);

/* Show a yes/no dialog. Returns 1 for yes, 0 for no. */
int tui_yes_no(const char *title, const char *question);

/* Show an input box. Edits buffer in place. Returns 1 on OK, 0 on cancel. */
int tui_input_box(const char *title, const char *prompt,
                  char *buffer, int max_len);

/* ================================================================
 * API - Installer progress screen
 * ================================================================ */

/* Begin progress screen: clear, draw bordered box with title. */
void tui_progress_begin(const char *title);

/* Update step: redraw step list, highlight current. step 1..total. */
void tui_progress_step(int step, int total, const char *description);

/* Show final message, wait for key, restore screen. */
void tui_progress_finish(const char *message);

/* ================================================================
 * API - Low-level key reading (useful for custom UIs)
 * ================================================================ */

/* Read a single key press (blocks). Returns TUI_KEY_* or ASCII.
 * Automatically sets keyboard to raw mode while waiting. */
int tui_read_key(void);

/* Non-blocking version. Returns TUI_KEY_NONE if no key available. */
int tui_poll_key(void);
