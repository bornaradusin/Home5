/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: controller shortcuts. The player sets up, in the app, a combo (two
 * or more buttons held together) or a sequence (buttons in order) that runs
 * something in Home Assistant — a scene, a script, or a light/switch/… — and
 * a small background part (built with the PS5's ShellUI, outside the app)
 * reads the controller while a game is in front and fires it.
 *
 * This part is the model and its two files:
 *   - <data>/shortcuts.txt   what the app shows and edits (self-contained)
 *   - the reader's config (h5_shortcuts_write_reader), under a folder a PS5
 *     payload can read, with the shortcuts and the token the reader needs.
 * It is plain C11, so the host tests run it as the console does. */
#ifndef H5_SHORTCUTS_H
#define H5_SHORTCUTS_H
#include "h5.h"

/* The DualSense's buttons, as ScePadData reports them (so the reader's config
 * carries these masks unchanged). The PS button is left out on purpose: it
 * opens the system menu. */
enum H5Btn {
    H5BTN_L3       = 0x00000002u,
    H5BTN_R3       = 0x00000004u,
    H5BTN_OPTIONS  = 0x00000008u,
    H5BTN_UP       = 0x00000010u,
    H5BTN_RIGHT    = 0x00000020u,
    H5BTN_DOWN     = 0x00000040u,
    H5BTN_LEFT     = 0x00000080u,
    H5BTN_L2       = 0x00000100u,
    H5BTN_R2       = 0x00000200u,
    H5BTN_L1       = 0x00000400u,
    H5BTN_R1       = 0x00000800u,
    H5BTN_TRIANGLE = 0x00001000u,
    H5BTN_CIRCLE   = 0x00002000u,
    H5BTN_CROSS    = 0x00004000u,
    H5BTN_SQUARE   = 0x00008000u,
    H5BTN_TOUCH    = 0x00100000u,
};
#define H5BTN_ALL (H5BTN_L3 | H5BTN_R3 | H5BTN_OPTIONS | H5BTN_UP | H5BTN_RIGHT | H5BTN_DOWN | H5BTN_LEFT | \
                   H5BTN_L2 | H5BTN_R2 | H5BTN_L1 | H5BTN_R1 | H5BTN_TRIANGLE | H5BTN_CIRCLE | H5BTN_CROSS | \
                   H5BTN_SQUARE | H5BTN_TOUCH)

enum { H5_SHORTCUTS_MAX = 32, H5_SEQ_MAX = 6, H5_SC_NAME = 48 };

typedef enum H5Trigger { H5_TRIG_COMBO = 0, H5_TRIG_SEQUENCE = 1 } H5Trigger;
typedef enum H5Feedback { H5_FB_LIGHTBAR = 0, H5_FB_NOTIFY = 1, H5_FB_RUMBLE = 2, H5_FB_ALL = 3 } H5Feedback;

typedef struct H5Shortcut {
    char name[H5_SC_NAME];     /* "Movie lights" */
    char entity[128];          /* the Home Assistant entity it runs */
    char domain[24];           /* resolved from the entity and its kind */
    char service[24];
    char data[96];             /* JSON members for the service, or "" */
    char label[64];            /* "Scene · Movie night", for the list */
    uint8_t mode;              /* H5Trigger */
    uint8_t confirm;           /* H5Feedback */
    uint8_t len;               /* sequence length (combo: the button count) */
    uint32_t combo;            /* combo: the buttons OR'd together */
    uint32_t seq[H5_SEQ_MAX];  /* sequence: one button each, in order */
} H5Shortcut;

typedef struct H5Shortcuts {
    H5Shortcut items[H5_SHORTCUTS_MAX];
    int count;
    uint8_t enabled;           /* the master switch */
    uint16_t rumble_str;       /* rumble strength 0–255 (both motors) */
    uint16_t rumble_ms;        /* how long the rumble lasts */
} H5Shortcuts;

/* ---- the model ------------------------------------------------------------------------------ */
void h5_shortcuts_reset(H5Shortcuts *s);   /* none, master on */
int h5_shortcuts_load(H5Shortcuts *s, const char *path);   /* 0, or -1 with it empty */
int h5_shortcuts_save(const H5Shortcuts *s, const char *path);

/* A new shortcut, last; its index, or -1 when full. name/entity/label/action are set, the trigger isn't yet. */
int h5_shortcuts_add(H5Shortcuts *s, const char *name, const char *entity, const char *domain, const char *service,
                     const char *data, const char *label);
void h5_shortcuts_delete(H5Shortcuts *s, int index);

/* The buttons of a combo in a fixed order (L.. R.. then face), up to H5_SEQ_MAX; the count. */
int h5_combo_buttons(uint32_t combo, uint32_t *out, int max);
/* How many buttons are set in a combo. */
int h5_btn_count(uint32_t mask);
/* A button's short name ("L3", "□"→"SQUARE") for the file and its glyph, or "" for none/several. */
const char *h5_btn_name(uint32_t one);

/* ---- the reader's config ---------------------------------------------------------------------
 * dir is a folder a PS5 payload can read (/data/homebrew/<title id>). url is the Home Assistant
 * address; access/refresh/expires/client are the token the reader signs its calls with (a
 * long-lived token has an empty refresh). 0, or -1. Written only when the master switch is on and
 * there's a token; otherwise the file is removed (h5_shortcuts_clear_reader). */
int h5_shortcuts_write_reader(const H5Shortcuts *s, const char *dir, const char *url, const char *access,
                              const char *refresh, int64_t expires, const char *client);
void h5_shortcuts_clear_reader(const char *dir);

#endif
