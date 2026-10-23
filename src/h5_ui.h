/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5's screens: signing in, the rooms and their devices, a light, a
 * thermostat, the menu and the device picker. The UI owns the Home Assistant
 * client and the layout; the platform gives it the time, the console
 * keyboard and a place for its files. Everything here runs in the host tests
 * as it does on the console. */
#ifndef H5_UI_H
#define H5_UI_H
#include "h5_draw.h"
#include "h5_ha.h"
#include "h5_layout.h"

/* Buttons, as edges (pressed: repeats while a direction or L2/R2 is held). */
enum {
    H5_KEY_UP = 1u << 0, H5_KEY_DOWN = 1u << 1, H5_KEY_LEFT = 1u << 2, H5_KEY_RIGHT = 1u << 3,
    H5_KEY_CROSS = 1u << 4, H5_KEY_CIRCLE = 1u << 5, H5_KEY_TRIANGLE = 1u << 6, H5_KEY_SQUARE = 1u << 7,
    H5_KEY_OPTIONS = 1u << 8, H5_KEY_L1 = 1u << 9, H5_KEY_R1 = 1u << 10, H5_KEY_L2 = 1u << 11, H5_KEY_R2 = 1u << 12,
};

/* What the keyboard is for: names get capitals; the rest is typed as it is. */
enum { H5_TEXT_NAME, H5_TEXT_PASSWORD, H5_TEXT_USERNAME, H5_TEXT_ADDRESS, H5_TEXT_NUMBER };

typedef struct H5Input {
    uint32_t pressed;
} H5Input;

typedef struct H5Platform {
    void *context;
    uint64_t (*clock_ms)(void);
    int64_t (*unix_time)(void);
    int offset_s;                 /* the console's time zone and summer time, for the clock */
    /* Opens the console's keyboard for that kind of text; its answer comes back
     * through h5_ui_text. 0 or -1. */
    int (*keyboard)(void *context, const char *title, const char *text, int kind);
    /* A notification on the console (Home Assistant's own, as they come). */
    void (*notify)(void *context, const char *title, const char *message);
    void (*log)(const char *line);
    int circle_accepts;           /* Japanese-style confirm */
    const char *data_dir;         /* writable: the account, the layout, the settings */
    const char *const *usb_roots; /* where home5.txt (a long-lived token) may be; NULL ends */
} H5Platform;

typedef struct H5Ui H5Ui;

H5Ui *h5_ui_create(const H5Platform *platform, const char *fonts_path);
void h5_ui_destroy(H5Ui *ui);
/* One frame's input; 1 when the screen needs drawing again. May block a few
 * seconds for network work once its busy screen has been drawn. */
int h5_ui_update(H5Ui *ui, const H5Input *input);
void h5_ui_draw(H5Ui *ui, H5Canvas *canvas);
/* The keyboard's answer: ok 0 when it was cancelled. */
void h5_ui_text(H5Ui *ui, int ok, const char *text);

/* What the controller should show: the light bar's colour (0xRRGGBB, -1 the
 * console's own) and whether L2/R2 push back (a value is being set with them). */
int32_t h5_ui_light_bar(const H5Ui *ui);
int h5_ui_triggers(const H5Ui *ui);

/* For the tests. */
const char *h5_ui_screen(const H5Ui *ui);
H5Ha *h5_ui_ha(H5Ui *ui);
const H5View *h5_ui_view(const H5Ui *ui);
H5Layout *h5_ui_layout(H5Ui *ui);
/* The picker's focused entity, or "". */
const char *h5_ui_picked(const H5Ui *ui);

#endif
