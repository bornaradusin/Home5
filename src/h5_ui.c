/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5's screens (h5_ui.h). */
#include "h5_ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1920, H = 1080, MARGIN = 96, NAV_X = 96, NAV_W = 380, MAIN_X = 532, MAIN_W = 1292 };
enum { CARD_W = 412, CARD_H = 230, CARD_GAP = 28, GRID_Y = 330, GRID_ROWS = 2, NAV_ROWS = 7, LIST_ROWS = 9 };
enum { MAX_ITEMS = 1024, TOAST_MS = 3200, REPEAT_STEP = 10 };

/* The palette (0xAARRGGBB). */
#define C_BG 0xff0b0f17u
#define C_PANEL 0xff121826u
#define C_CARD 0xff161d2bu
#define C_CARD_ON 0xff1d2638u
#define C_TRACK 0xff2c3648u
#define C_TEXT 0xffeef2f8u
#define C_SOFT 0xffc7d0deu
#define C_DIM 0xffa9b4c6u
#define C_FAINT 0xff8f9bb0u
#define C_AMBER 0xffffc35au
#define C_ORANGE 0xffff9a62u
#define C_ACCENT 0xff6ea0ffu
#define C_ACCENT_TEXT 0xff9fc0ffu
#define C_GREEN 0xff3ecf8eu
#define C_WARN 0xffff8a80u
#define C_FOCUS 0xffffffffu

typedef enum Screen {
    S_SIGN_IN, S_CODE, S_BUSY, S_HOME, S_LIGHT, S_CLIMATE, S_MENU, S_PICK, S_CONFIRM, S_ABOUT
} Screen;
static const char *const screen_names[] = { "sign-in", "code", "busy", "home", "light", "climate", "menu", "pick",
                                            "confirm", "about" };

typedef enum Busy { B_NONE, B_SIGN_IN, B_CODE, B_CONNECT, B_USB, B_SIGN_OUT } Busy;
typedef enum Ask { A_NONE, A_ADDRESS, A_USERNAME, A_PASSWORD, A_CODE, A_RENAME_DEVICE, A_RENAME_ROOM, A_NEW_ROOM } Ask;
typedef enum Zone { Z_NAV, Z_GRID } Zone;
typedef enum Confirm { C_NONE, C_LOCK, C_RESET, C_SIGN_OUT, C_DELETE_ROOM } Confirm;
typedef enum Item {
    M_ADD, M_REMOVE, M_RENAME_DEVICE, M_RENAME_ROOM, M_MOVE_UP, M_MOVE_DOWN, M_NEW_ROOM, M_DELETE_ROOM, M_LIGHT_FOLLOW,
    M_RESET, M_ABOUT, M_SIGN_OUT
} Item;

struct H5Ui {
    H5Platform platform;
    H5Fonts fonts;
    int fonts_ok;
    H5Ha ha;
    H5Layout *layout;
    H5View view;
    uint32_t built_ha, layout_version, built_layout;
    char path_account[H5_PATH], path_layout[H5_PATH], path_settings[H5_PATH];
    int light_follow;
    Screen screen, menu_from;
    int dirty;
    uint64_t now;
    /* signing in */
    char address[300], username[128], password[256], code[32];
    int form_row;
    char form_error[H5_ERROR];
    Ask ask;
    /* busy work, done once its screen has been drawn */
    Busy busy;
    int busy_drawn;
    /* the connection */
    uint64_t retry_at;
    int retries;
    /* the rooms */
    int room, room_chosen;
    char room_id[64];
    Zone zone;
    int nav, nav_top, card, card_top;
    char focus_id[128];
    /* a value being set with L2/R2: what it's going to, until HA has caught up */
    char adjust_id[128];
    float adjust_value;
    uint64_t adjust_until;
    /* a device's own screen */
    char detail_id[128];
    int swatch, climate_row, climate_col;
    /* the menu */
    Item menu[16];
    int menu_count, menu_at, menu_top;
    /* the picker */
    int *pick;
    int pick_count, pick_at, pick_top;
    /* asking first */
    Confirm confirm;
    char confirm_id[128];
    /* a line at the bottom for a moment */
    char toast[200];
    uint64_t toast_until;
    char shown_error[H5_ERROR];
};

static void say(H5Ui *ui, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void say(H5Ui *ui, const char *format, ...)
{
    char line[600];
    va_list args;
    if (!ui->platform.log) return;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ui->platform.log(line);
}

static void toast(H5Ui *ui, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void toast(H5Ui *ui, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(ui->toast, sizeof(ui->toast), format, args);
    va_end(args);
    ui->toast_until = ui->now + TOAST_MS;
    ui->dirty = 1;
}

static void go(H5Ui *ui, Screen screen)
{
    ui->screen = screen;
    ui->dirty = 1;
}

/* ---- settings ----------------------------------------------------------------------------------- */

static void settings_load(H5Ui *ui)
{
    H5Buf text = { 0 };
    ui->light_follow = 1;
    if (h5_read_file(ui->path_settings, &text, 16 * 1024)) return;
    if (strstr((char *)text.data, "light_bar = off")) ui->light_follow = 0;
    h5_buf_free(&text);
}

static void settings_save(H5Ui *ui)
{
    char text[200];
    snprintf(text, sizeof(text), "# Home5's settings\nlight_bar = %s\n", ui->light_follow ? "on" : "off");
    if (h5_write_file(ui->path_settings, text, strlen(text))) say(ui, "H5 ui can't write %s", ui->path_settings);
}

static void layout_changed(H5Ui *ui)
{
    ui->layout_version++;
    if (h5_layout_save(ui->layout, ui->path_layout)) say(ui, "H5 ui can't write %s", ui->path_layout);
    ui->dirty = 1;
}

/* ---- the rooms ---------------------------------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void rebuild(H5Ui *ui)
{
    if (ui->built_ha == ui->ha.version && ui->built_layout == ui->layout_version && ui->view.rooms) return;
    h5_view_free(&ui->view);
    h5_layout_view(ui->layout, &ui->ha, &ui->view);
    ui->built_ha = ui->ha.version;
    ui->built_layout = ui->layout_version;
    /* The same room as before, by id (the first one until one is chosen). */
    ui->room = 0;
    for (int i = 0; ui->room_chosen && i < ui->view.room_count; i++)
        if (!strcmp(ui->view.rooms[i].id, ui->room_id)) ui->room = i;
    if (ui->view.room_count) h5_copy(ui->room_id, sizeof(ui->room_id), ui->view.rooms[ui->room].id);
    ui->dirty = 1;
}

static const H5Room *current_room(H5Ui *ui)
{
    return ui->view.room_count ? &ui->view.rooms[clampi(ui->room, 0, ui->view.room_count - 1)] : NULL;
}

typedef struct Contents {
    int cards[MAX_ITEMS], actions[MAX_ITEMS], readings[MAX_ITEMS];
    int card_count, action_count, reading_count;
} Contents;

static void contents(H5Ui *ui, Contents *out)
{
    const H5Room *room = current_room(ui);
    out->card_count = out->action_count = out->reading_count = 0;
    if (!room) return;
    for (int i = 0; i < room->count; i++) {
        int index = ui->view.items[room->first + i];
        H5Kind kind = ui->ha.entities[index].kind;
        if (kind < H5_SCENE && out->card_count < MAX_ITEMS) out->cards[out->card_count++] = index;
        else if (kind < H5_SENSOR && out->action_count < MAX_ITEMS) out->actions[out->action_count++] = index;
        else if (out->reading_count < MAX_ITEMS) out->readings[out->reading_count++] = index;
    }
}

static const char *entity_name(H5Ui *ui, const H5Entity *e)
{
    /* A few at a time are in use in one call (a card's name and a toast): a small ring of them. */
    static char names[4][96];
    static int next;
    const H5Room *room = current_room(ui);
    char *out = names[next++ & 3];
    h5_display_name(ui->layout, e, room ? room->name : NULL, out, sizeof(names[0]));
    return out;
}

static H5Entity *focused(H5Ui *ui)
{
    if (ui->screen == S_LIGHT || ui->screen == S_CLIMATE) return h5_ha_entity(&ui->ha, ui->detail_id);
    if (ui->screen != S_HOME || ui->zone != Z_GRID) return NULL;
    return ui->focus_id[0] ? h5_ha_entity(&ui->ha, ui->focus_id) : NULL;
}

/* Keep the focused card by its entity after the room changed under it. */
static void refocus(H5Ui *ui, const Contents *c)
{
    for (int i = 0; i < c->card_count; i++)
        if (!strcmp(ui->ha.entities[c->cards[i]].id, ui->focus_id)) {
            ui->card = i;
            return;
        }
    ui->card = clampi(ui->card, 0, c->card_count ? c->card_count - 1 : 0);
    if (c->card_count) h5_copy(ui->focus_id, sizeof(ui->focus_id), ui->ha.entities[c->cards[ui->card]].id);
    else ui->focus_id[0] = 0;
}

static void enter_room(H5Ui *ui, int room)
{
    Contents *c = h5_alloc(sizeof(Contents));
    ui->room = clampi(room, 0, ui->view.room_count ? ui->view.room_count - 1 : 0);
    ui->room_chosen = ui->view.room_count > 0;
    if (ui->view.room_count) h5_copy(ui->room_id, sizeof(ui->room_id), ui->view.rooms[ui->room].id);
    ui->card = ui->card_top = 0;
    ui->focus_id[0] = 0;
    if (c) {
        contents(ui, c);
        refocus(ui, c);
        h5_free(c);
    }
    ui->dirty = 1;
}

/* ---- doing things ---------------------------------------------------------------------------------- */

static void domain_of(const char *entity, char *out, size_t size)
{
    const char *dot = strchr(entity, '.');
    size_t n = dot ? (size_t)(dot - entity) : 0;
    if (n >= size) n = size - 1;
    memcpy(out, entity, n);
    out[n] = 0;
}

static void call(H5Ui *ui, H5Entity *e, const char *service, const char *data)
{
    char domain[32];
    domain_of(e->id, domain, sizeof(domain));
    if (ui->ha.phase != H5_READY || h5_ha_call(&ui->ha, domain, service, e->id, data)) toast(ui, "Not connected to Home Assistant.");
}

/* ✕ on a device. */
static void activate(H5Ui *ui, H5Entity *e)
{
    if (!e->available && e->kind != H5_SCENE && e->kind != H5_SCRIPT && e->kind != H5_BUTTON) {
        toast(ui, "%s is unavailable.", entity_name(ui, e));
        return;
    }
    switch (e->kind) {
    case H5_LIGHT:
        e->on = !e->on;   /* shown at once; HA's answer follows */
        call(ui, e, e->on ? "turn_on" : "turn_off", NULL);
        break;
    case H5_SWITCH:
    case H5_FAN:
        e->on = !e->on;
        call(ui, e, e->on ? "turn_on" : "turn_off", NULL);
        break;
    case H5_COVER:
        call(ui, e, "toggle", NULL);
        break;
    case H5_CLIMATE:
        h5_copy(ui->detail_id, sizeof(ui->detail_id), e->id);
        ui->climate_row = ui->climate_col = 0;
        go(ui, S_CLIMATE);
        break;
    case H5_LOCK:
        ui->confirm = C_LOCK;
        h5_copy(ui->confirm_id, sizeof(ui->confirm_id), e->id);
        go(ui, S_CONFIRM);
        break;
    case H5_MEDIA:
        call(ui, e, "media_play_pause", NULL);
        break;
    case H5_SCENE:
    case H5_SCRIPT:
        call(ui, e, "turn_on", NULL);
        toast(ui, "%s", entity_name(ui, e));
        break;
    case H5_BUTTON:
        call(ui, e, "press", NULL);
        toast(ui, "%s", entity_name(ui, e));
        break;
    default:
        break;
    }
    ui->dirty = 1;
}

/* Whether L2/R2 set something on this device. */
static int adjustable(const H5Entity *e)
{
    return e && e->available &&
           ((e->kind == H5_LIGHT && e->brightness >= 0) || (e->kind == H5_COVER && e->position >= 0) ||
            (e->kind == H5_FAN && e->percentage >= 0) || (e->kind == H5_CLIMATE && !h5_isnan(e->target)));
}

/* The value L2/R2 sets on a device: brightness, position or speed in percent, or a temperature. */
static float adjusted_value(const H5Entity *e)
{
    switch (e->kind) {
    case H5_LIGHT: return e->on ? (float)((e->brightness * 100 + 127) / 255) : 0;
    case H5_COVER: return e->position;
    case H5_FAN: return e->on ? e->percentage : 0;
    case H5_CLIMATE: return e->target;
    default: return 0;
    }
}

static void set_adjusted(H5Entity *e, float value)
{
    switch (e->kind) {
    case H5_LIGHT:
        e->on = value > 0;
        e->brightness = (int16_t)lroundf(value * 255 / 100);
        break;
    case H5_COVER: e->position = (int8_t)value; break;
    case H5_FAN:
        e->percentage = (int8_t)value;
        e->on = value > 0;
        break;
    case H5_CLIMATE: e->target = value; break;
    default: break;
    }
}

/* While presses come quicker than Home Assistant's answers, the value they
 * step from is the one asked for last, not an older one HA reports back. */
static void keep_adjusting(H5Ui *ui)
{
    if (!ui->adjust_until) return;
    H5Entity *e = h5_ha_entity(&ui->ha, ui->adjust_id);
    if (!e || ui->now >= ui->adjust_until) {
        ui->adjust_until = 0;
        return;
    }
    set_adjusted(e, ui->adjust_value);
}

/* L2 (-1) or R2 (+1) on a device: a step down or up. */
static void adjust(H5Ui *ui, H5Entity *e, int step)
{
    char data[96];
    if (!adjustable(e)) return;
    if (ui->adjust_until && !strcmp(ui->adjust_id, e->id)) set_adjusted(e, ui->adjust_value);
    switch (e->kind) {
    case H5_LIGHT: {
        int percent = e->on ? (e->brightness * 100 + 127) / 255 : 0;
        percent = clampi((percent + step * REPEAT_STEP) / REPEAT_STEP * REPEAT_STEP, 0, 100);
        if (step > 0 && percent < REPEAT_STEP) percent = REPEAT_STEP;
        e->on = percent > 0;
        e->brightness = (int16_t)(percent * 255 / 100);
        if (!percent) {
            call(ui, e, "turn_off", NULL);
        } else {
            snprintf(data, sizeof(data), "\"brightness_pct\":%d", percent);
            call(ui, e, "turn_on", data);
        }
        break;
    }
    case H5_COVER:
        e->position = (int8_t)clampi(e->position + step * REPEAT_STEP, 0, 100);
        snprintf(data, sizeof(data), "\"position\":%d", e->position);
        call(ui, e, "set_cover_position", data);
        break;
    case H5_FAN:
        e->percentage = (int8_t)clampi(e->percentage + step * REPEAT_STEP, 0, 100);
        e->on = e->percentage > 0;
        snprintf(data, sizeof(data), "\"percentage\":%d", e->percentage);
        call(ui, e, "set_percentage", data);
        break;
    case H5_CLIMATE: {
        float stepped = e->target + (float)step * (e->step > 0 ? e->step : 0.5f);
        if (!h5_isnan(e->min_temp) && stepped < e->min_temp) stepped = e->min_temp;
        if (!h5_isnan(e->max_temp) && stepped > e->max_temp) stepped = e->max_temp;
        e->target = stepped;
        snprintf(data, sizeof(data), "\"temperature\":%.1f", stepped);
        call(ui, e, "set_temperature", data);
        break;
    }
    default:
        break;
    }
    h5_copy(ui->adjust_id, sizeof(ui->adjust_id), e->id);
    ui->adjust_value = adjusted_value(e);
    ui->adjust_until = ui->now + 1500;
    ui->dirty = 1;
}

/* ---- the light's colours ------------------------------------------------------------------------------ */

typedef struct Swatch { const char *name; int kelvin; int32_t rgb; uint32_t shown; } Swatch;

static const Swatch whites[] = {
    { "Warm white · 2700 K", 2700, -1, 0xffffd9a0u },
    { "Neutral white · 4000 K", 4000, -1, 0xfffff1dcu },
    { "Cool white · 6500 K", 6500, -1, 0xffdbe8ffu },
};
static const Swatch colours[] = {
    { "Sunset", 0, 0xff9a62, 0xffff9a62u }, { "Ocean", 0, 0x4cc3ff, 0xff4cc3ffu },
    { "Forest", 0, 0x5fd38d, 0xff5fd38du }, { "Violet", 0, 0xb18cff, 0xffb18cffu },
};

static int swatches(const H5Entity *e, Swatch *out)
{
    int n = 0;
    if (e->temp)
        for (size_t i = 0; i < 3; i++) out[n++] = whites[i];
    if (e->color)
        for (size_t i = 0; i < 4; i++) out[n++] = colours[i];
    return n;
}

static void apply_swatch(H5Ui *ui, H5Entity *e, const Swatch *s)
{
    char data[96];
    if (s->kelvin) {
        int k = clampi(s->kelvin, e->min_kelvin > 0 ? e->min_kelvin : 2000, e->max_kelvin > 0 ? e->max_kelvin : 6500);
        snprintf(data, sizeof(data), "\"color_temp_kelvin\":%d", k);
    } else {
        snprintf(data, sizeof(data), "\"rgb_color\":[%d,%d,%d]", (int)(s->rgb >> 16 & 255), (int)(s->rgb >> 8 & 255),
                 (int)(s->rgb & 255));
        e->rgb = s->rgb;
    }
    e->on = 1;
    call(ui, e, "turn_on", data);
}

/* The colour a light shows, for its picture and the light bar. */
static uint32_t light_colour(const H5Entity *e)
{
    if (e->rgb >= 0) return 0xff000000u | (uint32_t)e->rgb;
    if (e->kelvin >= 5000) return 0xffdbe8ffu;
    if (e->kelvin >= 3500) return 0xfffff1dcu;
    return 0xffffd9a0u;
}

/* ---- the menu ------------------------------------------------------------------------------------------- */

static void open_menu(H5Ui *ui)
{
    const H5Room *room = current_room(ui);
    H5Entity *e = ui->screen == S_HOME ? focused(ui) : NULL;
    ui->menu_count = 0;
    if (ui->ha.phase == H5_READY && room) {
        ui->menu[ui->menu_count++] = M_ADD;
        if (e) {
            ui->menu[ui->menu_count++] = M_REMOVE;
            ui->menu[ui->menu_count++] = M_RENAME_DEVICE;
        }
        ui->menu[ui->menu_count++] = M_RENAME_ROOM;
        if (ui->room > 0) ui->menu[ui->menu_count++] = M_MOVE_UP;
        if (ui->room + 1 < ui->view.room_count) ui->menu[ui->menu_count++] = M_MOVE_DOWN;
        ui->menu[ui->menu_count++] = M_NEW_ROOM;
        if (room->custom) ui->menu[ui->menu_count++] = M_DELETE_ROOM;
    }
    ui->menu[ui->menu_count++] = M_LIGHT_FOLLOW;
    if (ui->ha.phase == H5_READY) ui->menu[ui->menu_count++] = M_RESET;
    ui->menu[ui->menu_count++] = M_ABOUT;
    ui->menu[ui->menu_count++] = M_SIGN_OUT;
    ui->menu_at = ui->menu_top = 0;
    ui->menu_from = ui->screen;
    go(ui, S_MENU);
}

static void menu_text(H5Ui *ui, Item item, char *label, size_t size, char *about, size_t about_size, char *value,
                      size_t value_size)
{
    const H5Room *room = current_room(ui);
    H5Entity *e = h5_ha_entity(&ui->ha, ui->focus_id);
    const char *room_name = room ? room->name : "";
    const char *device = e ? entity_name(ui, e) : "";
    about[0] = value[0] = 0;
    switch (item) {
    case M_ADD:
        snprintf(label, size, "Add a device to %s", room_name);
        h5_copy(about, about_size, "Any device from Home Assistant; it stays in its own room too");
        break;
    case M_REMOVE:
        snprintf(label, size, "Remove %s from %s", device, room_name);
        h5_copy(about, about_size, "Only here on the PS5; Home Assistant is unchanged");
        break;
    case M_RENAME_DEVICE:
        snprintf(label, size, "Rename %s", device);
        h5_copy(about, about_size, "The name on this PS5");
        break;
    case M_RENAME_ROOM:
        snprintf(label, size, "Rename %s", room_name);
        h5_copy(about, about_size, "The name on this PS5");
        break;
    case M_MOVE_UP: snprintf(label, size, "Move %s up", room_name); break;
    case M_MOVE_DOWN: snprintf(label, size, "Move %s down", room_name); break;
    case M_NEW_ROOM:
        h5_copy(label, size, "New room");
        h5_copy(about, about_size, "A room of your own on this PS5");
        break;
    case M_DELETE_ROOM: snprintf(label, size, "Delete %s", room_name); break;
    case M_LIGHT_FOLLOW:
        h5_copy(label, size, "Light bar follows lights");
        h5_copy(about, about_size, "The controller glows in the colour of the light you're on");
        h5_copy(value, value_size, ui->light_follow ? "On" : "Off");
        break;
    case M_RESET:
        h5_copy(label, size, "Reset rooms");
        h5_copy(about, about_size, "Back to Home Assistant's rooms, names and order");
        break;
    case M_ABOUT:
        h5_copy(label, size, "About Home5");
        break;
    case M_SIGN_OUT:
        h5_copy(label, size, "Sign out");
        h5_copy(about, about_size, ui->ha.address);
        break;
    }
}

static void keyboard(H5Ui *ui, Ask ask, const char *title, const char *text, int kind)
{
    ui->ask = ask;
    if (!ui->platform.keyboard || ui->platform.keyboard(ui->platform.context, title, text, kind)) {
        ui->ask = A_NONE;
        toast(ui, "The keyboard didn't open.");
    }
}

static void build_pick(H5Ui *ui);

static void menu_choose(H5Ui *ui)
{
    const H5Room *room = current_room(ui);
    H5Entity *e = h5_ha_entity(&ui->ha, ui->focus_id);
    Item item = ui->menu[clampi(ui->menu_at, 0, ui->menu_count - 1)];
    char room_id[64];
    h5_copy(room_id, sizeof(room_id), room ? room->id : "");
    switch (item) {
    case M_ADD:
        build_pick(ui);
        go(ui, S_PICK);
        break;
    case M_REMOVE:
        if (e && room) {
            h5_layout_remove(ui->layout, room->id, e->id, h5_layout_is_added(ui->layout, room->id, e->id));
            toast(ui, "%s is no longer in %s here.", entity_name(ui, e), room->name);
            layout_changed(ui);
        }
        go(ui, S_HOME);
        break;
    case M_RENAME_DEVICE:
        if (e) keyboard(ui, A_RENAME_DEVICE, "Name on this PS5 (empty: Home Assistant's)", entity_name(ui, e), H5_TEXT_NAME);
        go(ui, S_HOME);
        break;
    case M_RENAME_ROOM:
        if (room) keyboard(ui, A_RENAME_ROOM, "Room name on this PS5", room->name, H5_TEXT_NAME);
        go(ui, S_HOME);
        break;
    case M_MOVE_UP:
    case M_MOVE_DOWN:
        if (room) {
            h5_layout_move_room(ui->layout, &ui->view, room->id, item == M_MOVE_UP ? -1 : 1);
            layout_changed(ui);
            rebuild(ui);
            ui->nav = ui->room;
        }
        go(ui, S_HOME);
        break;
    case M_NEW_ROOM:
        keyboard(ui, A_NEW_ROOM, "New room's name", "", H5_TEXT_NAME);
        go(ui, S_HOME);
        break;
    case M_DELETE_ROOM:
        ui->confirm = C_DELETE_ROOM;
        h5_copy(ui->confirm_id, sizeof(ui->confirm_id), room_id);
        go(ui, S_CONFIRM);
        break;
    case M_LIGHT_FOLLOW:
        ui->light_follow = !ui->light_follow;
        settings_save(ui);
        ui->dirty = 1;
        break;
    case M_RESET:
        ui->confirm = C_RESET;
        go(ui, S_CONFIRM);
        break;
    case M_ABOUT:
        go(ui, S_ABOUT);
        break;
    case M_SIGN_OUT:
        ui->confirm = C_SIGN_OUT;
        go(ui, S_CONFIRM);
        break;
    }
}

/* ---- the picker: every device, by its Home Assistant room ---------------------------------------------------- */

static int pick_order(const void *pa, const void *pb, void *context);
static H5Ui *pick_ui;

static int compare_pick(const void *pa, const void *pb)
{
    return pick_order(pa, pb, pick_ui);
}

static int pick_order(const void *pa, const void *pb, void *context)
{
    H5Ui *ui = context;
    const H5Entity *a = &ui->ha.entities[*(const int *)pa], *b = &ui->ha.entities[*(const int *)pb];
    const H5Area *aa = h5_ha_area(&ui->ha, a->area), *ab = h5_ha_area(&ui->ha, b->area);
    int c = h5_casecmp(aa ? aa->name : "~", ab ? ab->name : "~");
    if (c) return c;
    if (a->kind != b->kind) return (int)a->kind - (int)b->kind;
    return h5_casecmp(a->name, b->name);
}

static void build_pick(H5Ui *ui)
{
    h5_free(ui->pick);
    ui->pick = h5_alloc(sizeof(int) * (size_t)(ui->ha.count ? ui->ha.count : 1));
    ui->pick_count = 0;
    if (!ui->pick) return;
    for (int i = 0; i < ui->ha.count; i++)
        if (!ui->ha.entities[i].hidden) ui->pick[ui->pick_count++] = i;
    pick_ui = ui;
    qsort(ui->pick, (size_t)ui->pick_count, sizeof(int), compare_pick);
    ui->pick_at = ui->pick_top = 0;
}

static int in_room(H5Ui *ui, const H5Room *room, int entity)
{
    for (int i = 0; i < room->count; i++)
        if (ui->view.items[room->first + i] == entity) return 1;
    return 0;
}

/* ---- signing in ---------------------------------------------------------------------------------------------- */

static void start_busy(H5Ui *ui, Busy busy)
{
    ui->busy = busy;
    ui->busy_drawn = 0;
    go(ui, S_BUSY);
}

static void connected(H5Ui *ui)
{
    if (h5_ha_save(&ui->ha, ui->path_account)) say(ui, "H5 ui can't write the account");
    ui->retries = 0;
    memset(ui->password, 0, sizeof(ui->password));
    ui->code[0] = 0;
    ui->form_error[0] = 0;
    ui->zone = Z_GRID;
    go(ui, S_HOME);
}

/* home5.txt on a USB drive: "address = …" and "token = …" (a long-lived access token). */
static int token_from_usb(H5Ui *ui)
{
    for (const char *const *root = ui->platform.usb_roots; root && *root; root++) {
        char path[H5_PATH], address[300] = "", token[2048] = "";
        H5Buf text = { 0 };
        if (h5_join(path, sizeof(path), *root, "home5.txt") || h5_read_file(path, &text, 64 * 1024)) continue;
        for (char *line = (char *)text.data, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            char *name = h5_trim(line), *value = h5_trim(eq + 1);
            if (!strcmp(name, "address")) h5_copy(address, sizeof(address), value);
            else if (!strcmp(name, "token")) h5_copy(token, sizeof(token), value);
        }
        memset(text.data, 0, text.cap);
        h5_buf_free(&text);
        say(ui, "H5 ui home5.txt at %s address=%s token=%d", *root, address, token[0] != 0);
        int ok = address[0] && token[0] ? h5_ha_use_token(&ui->ha, address, token) : -1;
        memset(token, 0, sizeof(token));
        if (ok) {
            h5_format(ui->form_error, sizeof(ui->form_error), "%s",
                      address[0] && token[0] ? ui->ha.error : "home5.txt needs an \"address =\" and a \"token =\" line.");
            return -1;
        }
        return 0;
    }
    h5_copy(ui->form_error, sizeof(ui->form_error), "No home5.txt on a USB drive.");
    return -1;
}

/* The work behind the busy screen, now that it shows. */
static void do_busy(H5Ui *ui)
{
    Busy busy = ui->busy;
    ui->busy = B_NONE;
    switch (busy) {
    case B_SIGN_IN:
    case B_CODE: {
        H5Login result = busy == B_SIGN_IN ? h5_ha_login(&ui->ha, ui->address, ui->username, ui->password)
                                           : h5_ha_login_code(&ui->ha, ui->code);
        memset(ui->password, 0, sizeof(ui->password));
        if (result == H5_LOGIN_CODE) {
            h5_copy(ui->form_error, sizeof(ui->form_error), ui->ha.error);
            ui->form_row = 0;
            go(ui, S_CODE);
            return;
        }
        if (result != H5_LOGIN_OK) {
            h5_copy(ui->form_error, sizeof(ui->form_error), ui->ha.error);
            go(ui, S_SIGN_IN);
            return;
        }
        h5_copy(ui->address, sizeof(ui->address), ui->ha.address);
        say(ui, "H5 ui signed in to %s", ui->ha.address);
        /* Kept now, so a broken first connection doesn't mean signing in again. */
        if (h5_ha_save(&ui->ha, ui->path_account)) say(ui, "H5 ui can't write the account");
        start_busy(ui, B_CONNECT);
        return;
    }
    case B_CONNECT:
        if (h5_ha_connect(&ui->ha)) {
            if (ui->ha.phase == H5_SIGNED_OUT) {
                h5_copy(ui->form_error, sizeof(ui->form_error), ui->ha.error);
                go(ui, S_SIGN_IN);
                return;
            }
            /* Try again a little later, longer each time. */
            ui->retries++;
            ui->retry_at = ui->now + (ui->retries < 3 ? 5000u : ui->retries < 6 ? 15000u : 30000u);
            say(ui, "H5 ui connect failed (%s); again in %llu ms", ui->ha.error,
                (unsigned long long)(ui->retry_at - ui->now));
            go(ui, S_HOME);
            return;
        }
        connected(ui);
        return;
    case B_USB:
        if (token_from_usb(ui)) {
            go(ui, S_SIGN_IN);
            return;
        }
        h5_copy(ui->address, sizeof(ui->address), ui->ha.address);
        if (h5_ha_connect(&ui->ha)) {
            h5_copy(ui->form_error, sizeof(ui->form_error), ui->ha.error);
            ui->ha.phase = H5_SIGNED_OUT;
            go(ui, S_SIGN_IN);
            return;
        }
        connected(ui);
        toast(ui, "Signed in with home5.txt. You can delete it from the USB drive now.");
        return;
    case B_SIGN_OUT:
        h5_ha_sign_out(&ui->ha);
        remove(ui->path_account);
        rebuild(ui);
        ui->form_row = 0;
        ui->form_error[0] = 0;
        go(ui, S_SIGN_IN);
        return;
    case B_NONE:
        return;
    }
}

void h5_ui_text(H5Ui *ui, int ok, const char *text)
{
    Ask ask = ui->ask;
    const H5Room *room = current_room(ui);
    H5Entity *e = h5_ha_entity(&ui->ha, ui->focus_id);
    char id[64];
    ui->ask = A_NONE;
    ui->dirty = 1;
    if (!ok) return;
    switch (ask) {
    case A_ADDRESS: h5_copy(ui->address, sizeof(ui->address), text); break;
    case A_USERNAME: h5_copy(ui->username, sizeof(ui->username), text); break;
    case A_PASSWORD: h5_copy(ui->password, sizeof(ui->password), text); break;
    case A_CODE: h5_copy(ui->code, sizeof(ui->code), text); break;
    case A_RENAME_DEVICE:
        if (e) {
            /* Back to HA's own name when it's cleared or set to that. */
            h5_layout_rename(ui->layout, e->id, strcmp(text, e->name) ? text : "");
            layout_changed(ui);
        }
        break;
    case A_RENAME_ROOM:
        if (room && text[0]) {
            const H5Area *area = h5_ha_area(&ui->ha, room->id);
            h5_layout_rename(ui->layout, room->id[0] ? room->id : "", area && !strcmp(area->name, text) ? "" : text);
            layout_changed(ui);
        }
        break;
    case A_NEW_ROOM:
        if (text[0] && !h5_layout_new_room(ui->layout, text, id, sizeof(id))) {
            layout_changed(ui);
            rebuild(ui);
            for (int i = 0; i < ui->view.room_count; i++)
                if (!strcmp(ui->view.rooms[i].id, id)) enter_room(ui, i);
            ui->nav = ui->room;
            ui->zone = Z_NAV;
            toast(ui, "%s is ready. Press OPTIONS to add devices to it.", text);
        }
        break;
    case A_NONE:
        break;
    }
}

/* ---- input, screen by screen --------------------------------------------------------------------------------- */

static uint32_t accept_key(H5Ui *ui) { return ui->platform.circle_accepts ? H5_KEY_CIRCLE : H5_KEY_CROSS; }
static uint32_t back_key(H5Ui *ui) { return ui->platform.circle_accepts ? H5_KEY_CROSS : H5_KEY_CIRCLE; }

static int list_move(int *index, int count, uint32_t k)
{
    int before = *index;
    if (count <= 0) { *index = 0; return 0; }
    if (k & H5_KEY_UP) *index = *index > 0 ? *index - 1 : 0;
    if (k & H5_KEY_DOWN) *index = *index + 1 < count ? *index + 1 : count - 1;
    return *index != before;
}

static void scroll_to(int at, int *top, int rows)
{
    if (at < *top) *top = at;
    if (at >= *top + rows) *top = at - rows + 1;
    if (*top < 0) *top = 0;
}

static void update_sign_in(H5Ui *ui, uint32_t k)
{
    /* Rows: address, username, password, Sign in, token from USB. */
    if (list_move(&ui->form_row, 5, k)) ui->dirty = 1;
    if (k & H5_KEY_OPTIONS) {
        ui->form_row = 3;
        k |= accept_key(ui);
    }
    if (!(k & accept_key(ui))) return;
    switch (ui->form_row) {
    case 0: keyboard(ui, A_ADDRESS, "Home Assistant address, e.g. http://192.168.1.20:8123", ui->address, H5_TEXT_ADDRESS); break;
    case 1: keyboard(ui, A_USERNAME, "Home Assistant username", ui->username, H5_TEXT_USERNAME); break;
    case 2: keyboard(ui, A_PASSWORD, "Password", "", H5_TEXT_PASSWORD); break;
    case 3:
        if (!ui->address[0] || !ui->username[0] || !ui->password[0]) {
            h5_copy(ui->form_error, sizeof(ui->form_error), "Fill in the address, username and password first.");
            ui->form_row = !ui->address[0] ? 0 : !ui->username[0] ? 1 : 2;
            ui->dirty = 1;
            break;
        }
        ui->form_error[0] = 0;
        start_busy(ui, B_SIGN_IN);
        break;
    case 4:
        ui->form_error[0] = 0;
        start_busy(ui, B_USB);
        break;
    }
}

static void update_code(H5Ui *ui, uint32_t k)
{
    if (list_move(&ui->form_row, 2, k)) ui->dirty = 1;
    if (k & back_key(ui)) {
        ui->ha.flow[0] = 0;
        go(ui, S_SIGN_IN);
        return;
    }
    if (k & H5_KEY_OPTIONS) {
        ui->form_row = 1;
        k |= accept_key(ui);
    }
    if (!(k & accept_key(ui))) return;
    if (ui->form_row == 0) keyboard(ui, A_CODE, "The code from your authenticator app", "", H5_TEXT_NUMBER);
    else if (ui->code[0]) start_busy(ui, B_CODE);
}

static void update_home(H5Ui *ui, uint32_t k)
{
    Contents *c = h5_alloc(sizeof(Contents));
    if (!c) return;
    contents(ui, c);
    refocus(ui, c);
    int rooms = ui->view.room_count, nav_count = rooms + c->action_count;

    if (k & H5_KEY_OPTIONS) {
        open_menu(ui);
        goto out;
    }
    if (ui->ha.phase != H5_READY) {
        /* ✕: don't wait for the next try. */
        if ((k & accept_key(ui)) && ui->ha.phase == H5_OFFLINE) ui->retry_at = ui->now;
        goto out;
    }
    if (k & (H5_KEY_L1 | H5_KEY_R1)) {
        int to = clampi(ui->room + ((k & H5_KEY_R1) ? 1 : -1), 0, rooms ? rooms - 1 : 0);
        if (to != ui->room) {
            enter_room(ui, to);
            ui->nav = to;
            contents(ui, c);
            ui->zone = c->card_count ? Z_GRID : Z_NAV;
        }
        goto out;
    }
    if (ui->zone == Z_NAV) {
        int before = ui->nav;
        ui->nav = clampi(ui->nav, 0, nav_count ? nav_count - 1 : 0);
        list_move(&ui->nav, nav_count, k);
        if (ui->nav != before) {
            ui->dirty = 1;
            if (ui->nav < rooms && ui->nav != ui->room) enter_room(ui, ui->nav);
        }
        if ((k & H5_KEY_RIGHT) && c->card_count) {
            ui->zone = Z_GRID;
            ui->dirty = 1;
        }
        if (k & accept_key(ui)) {
            if (ui->nav < rooms) {
                if (c->card_count) ui->zone = Z_GRID;
            } else {
                activate(ui, &ui->ha.entities[c->actions[ui->nav - rooms]]);
            }
            ui->dirty = 1;
        }
        goto out;
    }
    /* the cards: three across */
    if (!c->card_count) {
        ui->zone = Z_NAV;
        goto out;
    }
    int before = ui->card;
    if ((k & H5_KEY_LEFT) && ui->card % 3 == 0) {
        ui->zone = Z_NAV;
        ui->nav = ui->room;
        ui->dirty = 1;
        goto out;
    }
    if (k & H5_KEY_LEFT) ui->card--;
    if ((k & H5_KEY_RIGHT) && ui->card + 1 < c->card_count) ui->card++;
    if ((k & H5_KEY_UP) && ui->card >= 3) ui->card -= 3;
    if ((k & H5_KEY_DOWN) && ui->card + 3 < c->card_count) ui->card += 3;
    else if ((k & H5_KEY_DOWN) && ui->card / 3 < (c->card_count - 1) / 3) ui->card = c->card_count - 1;
    if (ui->card != before) {
        h5_copy(ui->focus_id, sizeof(ui->focus_id), ui->ha.entities[c->cards[ui->card]].id);
        ui->dirty = 1;
    }
    H5Entity *e = &ui->ha.entities[c->cards[ui->card]];
    if (k & accept_key(ui)) activate(ui, e);
    if (k & H5_KEY_TRIANGLE) {
        if (e->kind == H5_LIGHT || e->kind == H5_CLIMATE) {
            h5_copy(ui->detail_id, sizeof(ui->detail_id), e->id);
            ui->climate_row = ui->climate_col = 0;
            Swatch list[8];
            int n = swatches(e, list);
            ui->swatch = 0;
            for (int i = 0; i < n; i++)
                if ((list[i].rgb >= 0 && list[i].rgb == e->rgb) || (list[i].kelvin && abs(list[i].kelvin - e->kelvin) < 300))
                    ui->swatch = i;
            go(ui, e->kind == H5_LIGHT ? S_LIGHT : S_CLIMATE);
        } else {
            toast(ui, "%s has nothing more to set.", entity_name(ui, e));
        }
    }
    if (k & H5_KEY_L2) adjust(ui, e, -1);
    if (k & H5_KEY_R2) adjust(ui, e, 1);
out:
    h5_free(c);
}

static void update_light(H5Ui *ui, uint32_t k)
{
    H5Entity *e = h5_ha_entity(&ui->ha, ui->detail_id);
    if (!e || (k & back_key(ui))) {
        go(ui, S_HOME);
        return;
    }
    Swatch list[8];
    int n = swatches(e, list);
    if (k & accept_key(ui)) activate(ui, e);
    if (k & H5_KEY_L2) adjust(ui, e, -1);
    if (k & H5_KEY_R2) adjust(ui, e, 1);
    if (n && (k & (H5_KEY_LEFT | H5_KEY_RIGHT))) {
        ui->swatch = clampi(ui->swatch + ((k & H5_KEY_RIGHT) ? 1 : -1), 0, n - 1);
        apply_swatch(ui, e, &list[ui->swatch]);
        ui->dirty = 1;
    }
    if (k & H5_KEY_SQUARE) {
        ui->light_follow = !ui->light_follow;
        settings_save(ui);
        ui->dirty = 1;
    }
    if (k & H5_KEY_OPTIONS) open_menu(ui);
}

static void update_climate(H5Ui *ui, uint32_t k)
{
    H5Entity *e = h5_ha_entity(&ui->ha, ui->detail_id);
    if (!e || (k & back_key(ui))) {
        go(ui, S_HOME);
        return;
    }
    int rows = e->preset_count ? 2 : 1;
    if (k & H5_KEY_UP) ui->climate_row = 0;
    if ((k & H5_KEY_DOWN) && rows > 1) ui->climate_row = 1;
    int count = ui->climate_row ? e->preset_count : e->mode_count;
    if (k & H5_KEY_LEFT) ui->climate_col--;
    if (k & H5_KEY_RIGHT) ui->climate_col++;
    ui->climate_col = clampi(ui->climate_col, 0, count ? count - 1 : 0);
    if (k & (H5_KEY_UP | H5_KEY_DOWN | H5_KEY_LEFT | H5_KEY_RIGHT)) ui->dirty = 1;
    if (k & H5_KEY_L2) adjust(ui, e, -1);
    if (k & H5_KEY_R2) adjust(ui, e, 1);
    if ((k & accept_key(ui)) && count) {
        char data[96];
        if (ui->climate_row == 0) {
            snprintf(data, sizeof(data), "\"hvac_mode\":\"%s\"", e->modes[ui->climate_col]);
            h5_copy(e->state, sizeof(e->state), e->modes[ui->climate_col]);
            e->on = strcmp(e->state, "off") != 0;
            call(ui, e, "set_hvac_mode", data);
        } else {
            snprintf(data, sizeof(data), "\"preset_mode\":\"%s\"", e->presets[ui->climate_col]);
            h5_copy(e->preset, sizeof(e->preset), e->presets[ui->climate_col]);
            call(ui, e, "set_preset_mode", data);
        }
        ui->dirty = 1;
    }
    if (k & H5_KEY_OPTIONS) open_menu(ui);
}

static void update_menu(H5Ui *ui, uint32_t k)
{
    if (list_move(&ui->menu_at, ui->menu_count, k)) ui->dirty = 1;
    scroll_to(ui->menu_at, &ui->menu_top, LIST_ROWS);
    if ((k & back_key(ui)) || (k & H5_KEY_OPTIONS)) {
        go(ui, ui->menu_from == S_MENU ? S_HOME : ui->menu_from);
        return;
    }
    if (k & accept_key(ui)) menu_choose(ui);
}

static void update_pick(H5Ui *ui, uint32_t k)
{
    const H5Room *room = current_room(ui);
    if (!room || (k & back_key(ui))) {
        go(ui, S_HOME);
        return;
    }
    if (list_move(&ui->pick_at, ui->pick_count, k)) ui->dirty = 1;
    if (k & H5_KEY_L1) ui->pick_at = clampi(ui->pick_at - LIST_ROWS, 0, ui->pick_count ? ui->pick_count - 1 : 0);
    if (k & H5_KEY_R1) ui->pick_at = clampi(ui->pick_at + LIST_ROWS, 0, ui->pick_count ? ui->pick_count - 1 : 0);
    if (k & (H5_KEY_L1 | H5_KEY_R1)) ui->dirty = 1;
    scroll_to(ui->pick_at, &ui->pick_top, LIST_ROWS);
    if ((k & accept_key(ui)) && ui->pick_count) {
        int index = ui->pick[ui->pick_at];
        H5Entity *e = &ui->ha.entities[index];
        char room_id[64], room_name[64];
        h5_copy(room_id, sizeof(room_id), room->id);
        h5_copy(room_name, sizeof(room_name), room->name);
        if (in_room(ui, room, index)) {
            h5_layout_remove(ui->layout, room_id, e->id, h5_layout_is_added(ui->layout, room_id, e->id));
            toast(ui, "Removed %s from %s.", e->name, room_name);
        } else {
            h5_layout_add(ui->layout, room_id, e->id);
            toast(ui, "Added %s to %s.", e->name, room_name);
        }
        layout_changed(ui);
        rebuild(ui);
    }
}

static void update_confirm(H5Ui *ui, uint32_t k)
{
    Confirm what = ui->confirm;
    if (k & back_key(ui)) {
        ui->confirm = C_NONE;
        go(ui, what == C_SIGN_OUT || what == C_RESET || what == C_DELETE_ROOM ? S_MENU : S_HOME);
        return;
    }
    if (!(k & accept_key(ui))) return;
    ui->confirm = C_NONE;
    switch (what) {
    case C_LOCK: {
        H5Entity *e = h5_ha_entity(&ui->ha, ui->confirm_id);
        if (e) call(ui, e, !strcmp(e->state, "locked") ? "unlock" : "lock", NULL);
        go(ui, S_HOME);
        break;
    }
    case C_RESET:
        h5_layout_reset(ui->layout);
        layout_changed(ui);
        rebuild(ui);
        enter_room(ui, 0);
        toast(ui, "The rooms are Home Assistant's again.");
        go(ui, S_HOME);
        break;
    case C_DELETE_ROOM:
        h5_layout_delete_room(ui->layout, ui->confirm_id);
        layout_changed(ui);
        rebuild(ui);
        enter_room(ui, 0);
        go(ui, S_HOME);
        break;
    case C_SIGN_OUT:
        start_busy(ui, B_SIGN_OUT);
        break;
    case C_NONE:
        go(ui, S_HOME);
        break;
    }
}

int h5_ui_update(H5Ui *ui, const H5Input *input)
{
    uint32_t k = input ? input->pressed : 0;
    ui->now = ui->platform.clock_ms ? ui->platform.clock_ms() : 0;

    if (ui->ask != A_NONE) return ui->dirty;   /* the keyboard is open */
    if (ui->screen == S_BUSY) {
        if (ui->busy_drawn) do_busy(ui);
        return 1;   /* the spinner turns */
    }
    /* The connection: what has come, and connecting again when it's time. */
    if (ui->ha.phase == H5_LOADING || ui->ha.phase == H5_READY) {
        if (h5_ha_poll(&ui->ha, 0) < 0) {
            ui->retries = 0;
            ui->retry_at = ui->now + 2000;
            ui->dirty = 1;
        }
    } else if (ui->ha.phase == H5_OFFLINE && ui->now >= ui->retry_at && ui->screen == S_HOME) {
        start_busy(ui, B_CONNECT);
        return 1;
    } else if (ui->ha.phase == H5_SIGNED_OUT && ui->screen != S_SIGN_IN && ui->screen != S_CODE) {
        h5_copy(ui->form_error, sizeof(ui->form_error), ui->ha.error);
        go(ui, S_SIGN_IN);
    }
    keep_adjusting(ui);
    if (ui->ha.error[0] && strcmp(ui->ha.error, ui->shown_error) && ui->ha.phase == H5_READY) {
        h5_copy(ui->shown_error, sizeof(ui->shown_error), ui->ha.error);
        toast(ui, "%s", ui->ha.error);
        ui->ha.error[0] = 0;
    }
    rebuild(ui);
    if (ui->toast_until && ui->now >= ui->toast_until) {
        ui->toast_until = 0;
        ui->dirty = 1;
    }
    if (ui->ha.phase == H5_OFFLINE && ui->screen == S_HOME) ui->dirty = 1;   /* the countdown */

    switch (ui->screen) {
    case S_SIGN_IN: update_sign_in(ui, k); break;
    case S_CODE: update_code(ui, k); break;
    case S_HOME: update_home(ui, k); break;
    case S_LIGHT: update_light(ui, k); break;
    case S_CLIMATE: update_climate(ui, k); break;
    case S_MENU: update_menu(ui, k); break;
    case S_PICK: update_pick(ui, k); break;
    case S_CONFIRM: update_confirm(ui, k); break;
    case S_ABOUT:
        if (k & (back_key(ui) | accept_key(ui))) go(ui, S_MENU);
        break;
    case S_BUSY: break;
    }
    int dirty = ui->dirty;
    ui->dirty = 0;
    return dirty;
}

/* ---- drawing: pieces ------------------------------------------------------------------------------------------ */

typedef enum Glyph { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_OPTIONS, G_L1R1, G_L2R2, G_LR } Glyph;

static int pill_key(H5Ui *ui, H5Canvas *c, const char *text, int x, int cy, int round)
{
    int w = h5_text_width(&ui->fonts, H5_FACE_KEY, text) + 22;
    h5_round_frame(c, (float)x, (float)cy - 16, (float)w, 32, round ? 16 : 8, 2, C_TEXT);
    h5_text(c, &ui->fonts, H5_FACE_KEY, x + w / 2, cy + 6, C_TEXT, text, 0, 1);
    return w;
}

static int button_glyph(H5Ui *ui, H5Canvas *c, Glyph glyph, int x, int y)
{
    const float r = 16, cx = (float)x + r, cy = (float)y - 8;
    switch (glyph) {
    case G_OPTIONS: return pill_key(ui, c, "OPTIONS", x, (int)cy, 1);
    case G_LR: return pill_key(ui, c, "← →", x, (int)cy, 1);
    case G_L1R1: {
        int w = pill_key(ui, c, "L1", x, (int)cy, 0);
        return w + 8 + pill_key(ui, c, "R1", x + w + 8, (int)cy, 0);
    }
    case G_L2R2: {
        int w = pill_key(ui, c, "L2", x, (int)cy, 0);
        return w + 8 + pill_key(ui, c, "R2", x + w + 8, (int)cy, 0);
    }
    default: break;
    }
    h5_ring(c, cx, cy, r, 2, C_TEXT);
    switch (glyph) {
    case G_CROSS:
        h5_line(c, cx - 6, cy - 6, cx + 6, cy + 6, 2.6f, C_TEXT);
        h5_line(c, cx - 6, cy + 6, cx + 6, cy - 6, 2.6f, C_TEXT);
        break;
    case G_CIRCLE: h5_ring(c, cx, cy, 7, 2.4f, C_TEXT); break;
    case G_SQUARE: h5_round_frame(c, cx - 6.5f, cy - 6.5f, 13, 13, 1.5f, 2.4f, C_TEXT); break;
    case G_TRIANGLE:
        h5_line(c, cx, cy - 7.5f, cx - 7, cy + 5, 2.4f, C_TEXT);
        h5_line(c, cx - 7, cy + 5, cx + 7, cy + 5, 2.4f, C_TEXT);
        h5_line(c, cx + 7, cy + 5, cx, cy - 7.5f, 2.4f, C_TEXT);
        break;
    default: break;
    }
    return (int)(2 * r);
}

typedef struct Hint { Glyph glyph; const char *label; } Hint;

static Glyph accept_glyph(H5Ui *ui) { return ui->platform.circle_accepts ? G_CIRCLE : G_CROSS; }
static Glyph back_glyph(H5Ui *ui) { return ui->platform.circle_accepts ? G_CROSS : G_CIRCLE; }

static void hints(H5Ui *ui, H5Canvas *c, int x, int y, const Hint *list, int count)
{
    for (int i = 0; i < count; i++) {
        x += button_glyph(ui, c, list[i].glyph, x, y) + 12;
        x += h5_text(c, &ui->fonts, H5_FACE_BODY, x, y, C_DIM, list[i].label, 0, 0) + 40;
    }
}

static void spinner(H5Canvas *c, float cx, float cy, float r, uint64_t now, uint32_t color)
{
    float a = (float)(now % 1100) / 1100.0f * 6.2831853f;
    h5_ring(c, cx, cy, r, 5, 0x30ffffffu);
    h5_arc(c, cx, cy, r, 5, a, a + 1.9f, color);
}

static void dim(H5Canvas *c, int alpha)
{
    h5_round_rect(c, 0, 0, W, H, 0, (uint32_t)alpha << 24);
}

/* Home5's mark: a house sending a signal (the app's icon). */
/* The icon's house and signal (art/make-art.py), size pixels square: the house is its outline grown
 * by 10 units with round corners, the signal two arcs around the roof's peak. */
static void mark(H5Canvas *c, float x, float y, float size, uint32_t bg)
{
    float s = size / 512;
    h5_triangle(c, x + 256 * s, y + 214 * s, x + 120 * s, y + 318 * s, x + 392 * s, y + 318 * s, 10 * s, C_TEXT);
    h5_round_rect(c, x + 142 * s, y + 308 * s, 228 * s, 130 * s, 10 * s, C_TEXT);
    h5_round_rect(c, x + 228 * s, y + 358 * s, 56 * s, 80 * s, 10 * s, bg);
    h5_arc(c, x + 256 * s, y + 214 * s, 64 * s, 26 * s, -0.785f, 0.785f, C_TEXT);
    h5_arc(c, x + 256 * s, y + 214 * s, 118 * s, 26 * s, -0.785f, 0.785f, C_DIM);
}

static void clock_text(H5Ui *ui, char *out, size_t size)
{
    out[0] = 0;
    if (!ui->platform.unix_time) return;
    int64_t t = ui->platform.unix_time() + ui->platform.offset_s, day = ((t % 86400) + 86400) % 86400;
    snprintf(out, size, "%02d:%02d", (int)(day / 3600), (int)(day / 60 % 60));
}

static void header(H5Ui *ui, H5Canvas *c, const char *crumb)
{
    char clock[16];
    mark(c, MARGIN - 6, 50, 64, C_BG);
    int x = MARGIN + 70;
    x += h5_text(c, &ui->fonts, H5_FACE_HEAD, x, 96, C_TEXT, "Home5", 0, 0);
    if (crumb) {
        h5_text(c, &ui->fonts, H5_FACE_TEXT, x + 18, 94, C_FAINT, crumb, 600, 0);
    } else {
        const char *status = ui->ha.phase == H5_READY ? "Home Assistant connected"
                             : ui->ha.phase == H5_LOADING ? "Loading your home…"
                             : ui->ha.phase == H5_OFFLINE ? "Not connected" : "Signed out";
        uint32_t dot = ui->ha.phase == H5_READY ? C_GREEN : ui->ha.phase == H5_LOADING ? C_AMBER : C_WARN;
        int tw = h5_text_width(&ui->fonts, H5_FACE_SMALL, status);
        float px = (float)x + 24;
        h5_round_rect(c, px, 64, (float)tw + 64, 42, 21, C_PANEL);
        h5_circle(c, px + 24, 85, 5, dot);
        h5_text(c, &ui->fonts, H5_FACE_SMALL, (int)px + 42, 92, C_DIM, status, 0, 0);
    }
    clock_text(ui, clock, sizeof(clock));
    if (clock[0]) h5_text(c, &ui->fonts, H5_FACE_HEAD, W - MARGIN, 96, C_DIM, clock, 0, 2);
}

/* The icons: 24-unit drawings scaled into size, like the design's. */
static void icon(H5Canvas *c, const H5Entity *e, float x, float y, float size, uint32_t col)
{
    float s = size / 24, w = 1.9f * s;
#define P(px, py) x + (px) * s, y + (py) * s
    const char *dc = e->device_class;
    H5Kind kind = e->kind;
    if (kind == H5_SENSOR && !strcmp(dc, "temperature")) kind = H5_CLIMATE;
    switch (kind) {
    case H5_LIGHT:
        h5_ring(c, P(12, 9.2f), 6 * s, w, col);
        h5_line(c, P(9.5f, 14.5f), P(9.5f, 17), w, col);
        h5_line(c, P(14.5f, 14.5f), P(14.5f, 17), w, col);
        h5_line(c, P(9, 18), P(15, 18), w, col);
        h5_line(c, P(10, 21), P(14, 21), w, col);
        break;
    case H5_SWITCH:
        h5_round_frame(c, P(3, 7.5f), 18 * s, 9 * s, 4.5f * s, w, col);
        h5_circle(c, P(e->on ? 16.5f : 7.5f, 12), 2.6f * s, col);
        break;
    case H5_FAN:
        h5_circle(c, P(12, 12), 2 * s, col);
        for (int i = 0; i < 3; i++) {
            float a = (float)i * 2.0944f;
            h5_arc(c, x + (12 + 4.5f * sinf(a)) * s, y + (12 - 4.5f * h5_cos(a)) * s, 4.2f * s, w, a - 1.6f, a + 1.0f, col);
        }
        break;
    case H5_COVER:
        h5_line(c, P(3, 4), P(21, 4), w, col);
        h5_line(c, P(5, 4), P(5, 15), w, col);
        h5_line(c, P(19, 4), P(19, 15), w, col);
        h5_line(c, P(5, 8), P(19, 8), w, col);
        h5_line(c, P(5, 12), P(19, 12), w, col);
        h5_line(c, P(12, 15), P(12, 20), w, col);
        break;
    case H5_CLIMATE:
        h5_round_frame(c, P(10, 2.5f), 4 * s, 12 * s, 2 * s, w, col);
        h5_ring(c, P(12, 17.5f), 3.6f * s, w, col);
        break;
    case H5_LOCK:
        h5_round_frame(c, P(5, 11), 14 * s, 10 * s, 2 * s, w, col);
        h5_arc(c, P(12, 8), 4 * s, w, -1.5708f, 1.5708f, col);
        h5_line(c, P(8, 8), P(8, 11), w, col);
        if (!strcmp(e->state, "locked")) h5_line(c, P(16, 8), P(16, 11), w, col);
        break;
    case H5_MEDIA:
        h5_round_frame(c, P(3, 5), 18 * s, 12 * s, 2 * s, w, col);
        h5_line(c, P(8, 21), P(16, 21), w, col);
        h5_line(c, P(12, 17), P(12, 21), w, col);
        break;
    case H5_BINARY:
        if (!strcmp(dc, "door") || !strcmp(dc, "window") || !strcmp(dc, "opening") || !strcmp(dc, "garage_door")) {
            h5_round_frame(c, P(6, 3), 12 * s, 18 * s, 1 * s, w, col);
            h5_circle(c, P(14.5f, 12), 1.2f * s, col);
        } else {
            h5_ring(c, P(12, 12), 3 * s, w, col);
            h5_arc(c, P(12, 12), 8 * s, w, 3.93f, 5.5f, col);
            h5_arc(c, P(12, 12), 8 * s, w, 0.79f, 2.36f, col);
        }
        break;
    case H5_SENSOR:
        if (!strcmp(dc, "power") || !strcmp(dc, "energy") || !strcmp(dc, "current") || !strcmp(dc, "voltage")) {
            h5_line(c, P(13, 2), P(5, 14), w, col);
            h5_line(c, P(5, 14), P(12, 14), w, col);
            h5_line(c, P(12, 14), P(11, 22), w, col);
            h5_line(c, P(11, 22), P(19, 10), w, col);
            h5_line(c, P(19, 10), P(12, 10), w, col);
            h5_line(c, P(12, 10), P(13, 2), w, col);
        } else if (!strcmp(dc, "humidity") || !strcmp(dc, "moisture")) {
            h5_arc(c, P(12, 14.5f), 5.5f * s, w, 2.2f, 7.2f, col);
            h5_line(c, P(12, 3), P(7.3f, 11.5f), w, col);
            h5_line(c, P(12, 3), P(16.7f, 11.5f), w, col);
        } else {
            h5_arc(c, P(12, 13), 8 * s, w, -2.1f, 2.1f, col);
            h5_line(c, P(12, 13), P(16, 9), w, col);
        }
        break;
    default:
        h5_ring(c, P(12, 12), 8 * s, w, col);
        h5_circle(c, P(12, 12), 2.5f * s, col);
        break;
    }
#undef P
}

/* A bar of fraction 0-1. */
static void bar(H5Canvas *c, float x, float y, float w, float h, float fraction, uint32_t col)
{
    h5_round_rect(c, x, y, w, h, h / 2, C_TRACK);
    if (fraction > 0) h5_round_rect(c, x, y, fmaxf(h, w * fminf(fraction, 1)), h, h / 2, col);
}

static uint32_t entity_colour(const H5Entity *e)
{
    if (!e->available) return C_FAINT;
    switch (e->kind) {
    case H5_LIGHT: return e->on ? (e->rgb >= 0 ? light_colour(e) : C_AMBER) : C_FAINT;
    case H5_CLIMATE: return !e->on ? C_FAINT : !strcmp(e->state, "cool") ? C_ACCENT : C_ORANGE;
    case H5_SWITCH:
    case H5_FAN:
    case H5_MEDIA: return e->on ? C_ACCENT : C_FAINT;
    case H5_LOCK: return !strcmp(e->state, "locked") ? C_GREEN : C_WARN;
    default: return C_SOFT;
    }
}

static void card(H5Ui *ui, H5Canvas *c, const H5Entity *e, float x, float y, int focus)
{
    char status[160];
    uint32_t col = entity_colour(e), text_col = col == C_FAINT ? C_FAINT : col == C_SOFT ? C_DIM : col;
    int lit = e->on && e->available;
    if (text_col == C_ACCENT) text_col = C_ACCENT_TEXT;
    if (focus) h5_round_rect(c, x - 4, y - 4, CARD_W + 8, CARD_H + 8, 28, C_FOCUS);
    h5_round_rect(c, x, y, CARD_W, CARD_H, 24, lit ? C_CARD_ON : C_CARD);
    icon(c, e, x + 32, y + 30, 44, col);
    h5_text(c, &ui->fonts, H5_FACE_CARD, (int)x + 32, (int)y + 136, C_TEXT, entity_name(ui, e), CARD_W - 64, 0);
    h5_entity_status(e, ui->ha.temp_unit, status, sizeof(status));
    h5_text(c, &ui->fonts, H5_FACE_BODY, (int)x + 32, (int)y + 172, text_col, status, CARD_W - 64, 0);
    float fraction = -1;
    if (e->kind == H5_LIGHT && e->brightness >= 0) fraction = e->on ? (float)e->brightness / 255 : 0;
    if (e->kind == H5_COVER && e->position >= 0) fraction = (float)e->position / 100;
    if (e->kind == H5_FAN && e->percentage >= 0) fraction = e->on ? (float)e->percentage / 100 : 0;
    if (fraction >= 0) bar(c, x + 32, y + 196, CARD_W - 64, 8, fraction, col == C_FAINT ? C_DIM : col);
}

static void toast_line(H5Ui *ui, H5Canvas *c)
{
    if (!ui->toast_until || ui->now >= ui->toast_until) return;
    int w = h5_text_width(&ui->fonts, H5_FACE_TEXT, ui->toast) + 64;
    if (w > 1400) w = 1400;
    h5_shadow(c, (W - w) / 2.0f, 926, (float)w, 64, 32, 30, 0x80000000u);
    h5_round_rect(c, (W - w) / 2.0f, 926, (float)w, 64, 32, 0xff26324du);
    h5_text(c, &ui->fonts, H5_FACE_TEXT, W / 2, 967, C_TEXT, ui->toast, w - 64, 1);
}

/* ---- drawing: the screens -------------------------------------------------------------------------------- */

static void form_field(H5Ui *ui, H5Canvas *c, float x, float y, const char *label, const char *value,
                       const char *placeholder, int focus)
{
    h5_text(c, &ui->fonts, H5_FACE_LABEL, (int)x, (int)y, C_SOFT, label, 0, 0);
    if (focus) h5_round_rect(c, x - 4, y + 16, 764, 84, 20, C_FOCUS);
    h5_round_rect(c, x, y + 20, 756, 76, 16, C_CARD_ON);
    h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)x + 26, (int)y + 68, value[0] ? C_TEXT : C_FAINT, value[0] ? value : placeholder,
            700, 0);
}

static void sign_in_screen(H5Ui *ui, H5Canvas *c)
{
    char dots[64] = "";
    float x = (W - 900) / 2.0f, y = 120;
    for (size_t i = 0; i < strlen(ui->password) && i < 20; i++) strcat(dots, "•");
    h5_round_rect(c, x, y, 900, 840, 32, C_PANEL);
    mark(c, x + 50, y + 40, 112, C_PANEL);
    h5_text(c, &ui->fonts, H5_FACE_HEAD, (int)x + 176, (int)y + 102, C_TEXT, "Connect to Home Assistant", 0, 0);
    h5_text(c, &ui->fonts, H5_FACE_BODY, (int)x + 176, (int)y + 140, C_DIM,
            "Home5 keeps a token on this PS5, never your password.", 680, 0);
    float fx = x + 72;
    form_field(ui, c, fx, y + 210, "Address", ui->address, "http://192.168.1.20:8123", ui->form_row == 0);
    form_field(ui, c, fx, y + 340, "Username", ui->username, "Your Home Assistant username", ui->form_row == 1);
    form_field(ui, c, fx, y + 470, "Password", dots, "Password", ui->form_row == 2);
    if (ui->form_row == 3) h5_round_rect(c, fx - 4, y + 604, 764, 88, 44, C_FOCUS);
    h5_round_rect(c, fx, y + 608, 756, 80, 40, C_ACCENT);
    h5_text(c, &ui->fonts, H5_FACE_CARD, W / 2, (int)y + 658, 0xff08101fu, "Sign in", 0, 1);
    if (ui->form_row == 4) h5_round_rect(c, fx - 4, y + 704, 764, 64, 32, C_FOCUS);
    h5_round_rect(c, fx, y + 708, 756, 56, 28, C_CARD);
    h5_text(c, &ui->fonts, H5_FACE_BODY, W / 2, (int)y + 744, C_SOFT, "Or use a token from home5.txt on a USB drive", 0, 1);
    if (ui->form_error[0])
        h5_text_wrap(c, &ui->fonts, H5_FACE_BODY, W / 2, (int)y + 806, 820, 2, 30, C_WARN, ui->form_error, 1);
    Hint list[] = { { accept_glyph(ui), ui->form_row < 3 ? "Type (the PS5 keyboard opens)" : "Choose" },
                    { G_OPTIONS, "Sign in" } };
    hints(ui, c, MARGIN, 1012, list, 2);
}

static void code_screen(H5Ui *ui, H5Canvas *c)
{
    float x = (W - 900) / 2.0f, y = 250;
    h5_round_rect(c, x, y, 900, 520, 32, C_PANEL);
    h5_text(c, &ui->fonts, H5_FACE_HEAD, (int)x + 72, (int)y + 100, C_TEXT, "Two-step sign-in", 0, 0);
    h5_text(c, &ui->fonts, H5_FACE_BODY, (int)x + 72, (int)y + 140, C_DIM,
            "Type the code your authenticator app shows for Home Assistant.", 760, 0);
    form_field(ui, c, x + 72, y + 210, "Code", ui->code, "123456", ui->form_row == 0);
    if (ui->form_row == 1) h5_round_rect(c, x + 68, y + 344, 764, 88, 44, C_FOCUS);
    h5_round_rect(c, x + 72, y + 348, 756, 80, 40, C_ACCENT);
    h5_text(c, &ui->fonts, H5_FACE_CARD, W / 2, (int)y + 398, 0xff08101fu, "Continue", 0, 1);
    if (ui->form_error[0]) h5_text(c, &ui->fonts, H5_FACE_BODY, W / 2, (int)y + 480, C_WARN, ui->form_error, 820, 1);
    Hint list[] = { { accept_glyph(ui), "Choose" }, { back_glyph(ui), "Back" } };
    hints(ui, c, MARGIN, 1012, list, 2);
}

static void busy_screen(H5Ui *ui, H5Canvas *c)
{
    const char *title = ui->busy == B_SIGN_IN || ui->busy == B_CODE ? "Signing in…"
                        : ui->busy == B_USB ? "Reading home5.txt…"
                        : ui->busy == B_SIGN_OUT ? "Signing out…" : "Connecting to Home Assistant…";
    mark(c, W / 2.0f - 90, 230, 180, C_BG);
    spinner(c, W / 2.0f, 560, 34, ui->now, C_TEXT);
    h5_text(c, &ui->fonts, H5_FACE_HEAD, W / 2, 680, C_TEXT, title, 0, 1);
    h5_text(c, &ui->fonts, H5_FACE_TEXT, W / 2, 730, C_DIM, ui->address[0] ? ui->address : ui->ha.address, 1200, 1);
}

static void room_summary(H5Ui *ui, const Contents *ct, char *out, size_t size)
{
    char part[96];
    int lights = 0;
    out[0] = 0;
    for (int i = 0; i < ct->reading_count; i++) {
        const H5Entity *e = &ui->ha.entities[ct->readings[i]];
        if (e->kind == H5_SENSOR && e->available &&
            (!strcmp(e->device_class, "temperature") || !strcmp(e->device_class, "humidity"))) {
            h5_entity_status(e, ui->ha.temp_unit, part, sizeof(part));
            if (!strcmp(e->device_class, "humidity")) strncat(part, " humidity", sizeof(part) - strlen(part) - 1);
            if (strstr(out, strcmp(e->device_class, "humidity") ? "°" : "humidity")) continue;
            if (out[0]) strncat(out, " · ", size - strlen(out) - 1);
            strncat(out, part, size - strlen(out) - 1);
        }
    }
    for (int i = 0; i < ct->card_count; i++) {
        const H5Entity *e = &ui->ha.entities[ct->cards[i]];
        lights += e->kind == H5_LIGHT && e->on;
    }
    snprintf(part, sizeof(part), lights == 1 ? "1 light on" : lights ? "%d lights on" : "Lights off", lights);
    if (out[0]) strncat(out, " · ", size - strlen(out) - 1);
    strncat(out, part, size - strlen(out) - 1);
}

static void home_screen(H5Ui *ui, H5Canvas *c)
{
    Contents *ct = h5_alloc(sizeof(Contents));
    char line[200];
    header(ui, c, NULL);
    if (!ct) return;
    contents(ui, ct);
    const H5Room *room = current_room(ui);
    int rooms = ui->view.room_count;

    if (ui->ha.phase == H5_READY && !rooms) {
        h5_text(c, &ui->fonts, H5_FACE_TITLE, W / 2, 470, C_TEXT, "Nothing to show yet", 0, 1);
        h5_text(c, &ui->fonts, H5_FACE_TEXT, W / 2, 530, C_DIM,
                "Put devices in areas in Home Assistant, or press OPTIONS to make a room here.", 0, 1);
    }
    /* the rooms */
    h5_text(c, &ui->fonts, H5_FACE_CAPS, NAV_X + 24, 226, C_FAINT, "ROOMS", 0, 0);
    int nav_focus = ui->zone == Z_NAV && ui->screen == S_HOME;
    if (ui->nav < rooms) scroll_to(ui->nav, &ui->nav_top, NAV_ROWS);
    for (int i = ui->nav_top; i < rooms && i < ui->nav_top + NAV_ROWS; i++) {
        const H5Room *r = &ui->view.rooms[i];
        float y = 246 + 82.0f * (float)(i - ui->nav_top);
        int on = 0;
        for (int k = 0; k < r->count; k++) {
            const H5Entity *e = &ui->ha.entities[ui->view.items[r->first + k]];
            on += (e->kind == H5_LIGHT || e->kind == H5_SWITCH || e->kind == H5_FAN) && e->on;
        }
        if (nav_focus && ui->nav == i) h5_round_rect(c, NAV_X - 4, y - 4, NAV_W + 8, 80, 20, C_FOCUS);
        if (i == ui->room) h5_round_rect(c, NAV_X, y, NAV_W, 72, 16, 0xff1d2638u);
        snprintf(line, sizeof(line), on ? "%d on" : "Off", on);
        int vw = h5_text(c, &ui->fonts, H5_FACE_BODY, NAV_X + NAV_W - 24, (int)y + 46, on ? C_AMBER : C_FAINT, line, 0, 2);
        h5_text(c, &ui->fonts, i == ui->room ? H5_FACE_LABEL : H5_FACE_TEXT, NAV_X + 24, (int)y + 46,
                i == ui->room ? C_TEXT : C_SOFT, r->name, NAV_W - 72 - vw, 0);
    }
    if (rooms > ui->nav_top + NAV_ROWS)
        h5_text(c, &ui->fonts, H5_FACE_SMALL, NAV_X + 24, 246 + 82 * NAV_ROWS + 4, C_FAINT, "More rooms below", 0, 0);
    /* its scenes and scripts */
    if (ct->action_count) {
        float y = 246 + 82.0f * (float)(rooms < NAV_ROWS ? rooms : NAV_ROWS) + 40;
        h5_text(c, &ui->fonts, H5_FACE_CAPS, NAV_X + 24, (int)y, C_FAINT, "SCENES", 0, 0);
        float px = NAV_X + 12, py = y + 18;
        for (int i = 0; i < ct->action_count && py < 960; i++) {
            const H5Entity *e = &ui->ha.entities[ct->actions[i]];
            const char *name = entity_name(ui, e);
            float w = (float)h5_text_width(&ui->fonts, H5_FACE_BODY, name) + 44;
            if (w > NAV_W - 12) w = NAV_W - 12;
            if (px + w > NAV_X + NAV_W + 12) {
                px = NAV_X + 12;
                py += 64;
                if (py >= 960) break;
            }
            if (nav_focus && ui->nav == rooms + i) h5_round_rect(c, px - 4, py - 4, w + 8, 60, 30, C_FOCUS);
            h5_round_rect(c, px, py, w, 52, 26, C_CARD);
            h5_text(c, &ui->fonts, H5_FACE_BODY, (int)px + 22, (int)py + 34, C_TEXT, name, (int)w - 44, 0);
            px += w + 12;
        }
    }
    /* the room */
    if (room) {
        h5_text(c, &ui->fonts, H5_FACE_TITLE, MAIN_X, 252, C_TEXT, room->name, MAIN_W, 0);
        room_summary(ui, ct, line, sizeof(line));
        h5_text(c, &ui->fonts, H5_FACE_TEXT, MAIN_X, 296, C_DIM, line, MAIN_W, 0);
        int rows = (ct->card_count + 2) / 3, row = ui->card / 3;
        if (ui->zone == Z_GRID) scroll_to(row, &ui->card_top, GRID_ROWS);
        if (ui->card_top > (rows > GRID_ROWS ? rows - GRID_ROWS : 0)) ui->card_top = rows > GRID_ROWS ? rows - GRID_ROWS : 0;
        for (int i = ui->card_top * 3; i < ct->card_count && i < (ui->card_top + GRID_ROWS) * 3; i++) {
            float x = MAIN_X + (float)(i % 3) * (CARD_W + CARD_GAP), y = GRID_Y + (float)(i / 3 - ui->card_top) * (CARD_H + CARD_GAP);
            card(ui, c, &ui->ha.entities[ct->cards[i]], x, y, ui->zone == Z_GRID && ui->card == i && ui->screen == S_HOME);
        }
        if (!ct->card_count && ct->reading_count == 0)
            h5_text(c, &ui->fonts, H5_FACE_TEXT, MAIN_X, GRID_Y + 60, C_DIM,
                    room->custom ? "No devices here yet. Press OPTIONS to add some." : "Nothing here to switch.", 0, 0);
        if (rows > ui->card_top + GRID_ROWS)
            h5_text(c, &ui->fonts, H5_FACE_SMALL, MAIN_X + MAIN_W, GRID_Y + GRID_ROWS * (CARD_H + CARD_GAP) - 6, C_FAINT,
                    "More below", 0, 2);
        /* readings */
        float x = MAIN_X;
        for (int i = 0; i < ct->reading_count && x < MAIN_X + MAIN_W - 200; i++) {
            const H5Entity *e = &ui->ha.entities[ct->readings[i]];
            char status[96], text[200];
            h5_entity_status(e, ui->ha.temp_unit, status, sizeof(status));
            snprintf(text, sizeof(text), "%s · %s", entity_name(ui, e), status);
            float w = (float)h5_text_width(&ui->fonts, H5_FACE_BODY, text) + 96;
            if (w > 520) w = 520;
            if (x + w > MAIN_X + MAIN_W) {
                snprintf(text, sizeof(text), "+%d more", ct->reading_count - i);
                h5_text(c, &ui->fonts, H5_FACE_BODY, (int)x + 12, 892, C_FAINT, text, 0, 0);
                break;
            }
            h5_round_rect(c, x, 846, w, 72, 18, C_PANEL);
            icon(c, e, x + 22, 866, 30, C_SOFT);
            h5_text(c, &ui->fonts, H5_FACE_BODY, (int)x + 66, 892, C_SOFT, text, (int)w - 88, 0);
            x += w + 20;
        }
    }
    /* not connected: a panel over it */
    if (ui->ha.phase != H5_READY && ui->screen == S_HOME) {
        float px = (W - 1000) / 2.0f, py = 360;
        dim(c, 160);
        h5_round_rect(c, px, py, 1000, 300, 28, C_PANEL);
        if (ui->ha.phase == H5_LOADING) {
            spinner(c, px + 110, py + 150, 34, ui->now, C_TEXT);
            h5_text(c, &ui->fonts, H5_FACE_HEAD, (int)px + 180, (int)py + 140, C_TEXT, "Loading your home…", 0, 0);
            h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)px + 180, (int)py + 190, C_DIM, ui->ha.address, 760, 0);
        } else {
            long wait = ui->retry_at > ui->now ? (long)((ui->retry_at - ui->now + 999) / 1000) : 0;
            h5_text(c, &ui->fonts, H5_FACE_HEAD, (int)px + 72, (int)py + 100, C_TEXT, "Can't reach Home Assistant", 0, 0);
            h5_text_wrap(c, &ui->fonts, H5_FACE_TEXT, (int)px + 72, (int)py + 150, 860, 2, 36, C_DIM,
                         ui->ha.error[0] ? ui->ha.error : ui->ha.address, 0);
            snprintf(line, sizeof(line), wait ? "Trying again in %ld s" : "Trying again…", wait);
            h5_text(c, &ui->fonts, H5_FACE_BODY, (int)px + 72, (int)py + 250, C_FAINT, line, 0, 0);
        }
    }
    /* the hints, for what's focused */
    if (ui->screen == S_HOME) {
        Hint list[6];
        int n = 0;
        H5Entity *e = focused(ui);
        if (ui->ha.phase != H5_READY) {
            if (ui->ha.phase == H5_OFFLINE) list[n++] = (Hint){ accept_glyph(ui), "Try now" };
            list[n++] = (Hint){ G_OPTIONS, "Menu" };
        } else if (ui->zone == Z_GRID && e) {
            list[n++] = (Hint){ accept_glyph(ui), e->kind == H5_CLIMATE ? "Open" : e->kind == H5_COVER ? "Open / close"
                                                  : e->kind == H5_LOCK ? (strcmp(e->state, "locked") ? "Lock" : "Unlock")
                                                  : e->kind == H5_MEDIA ? "Play / pause" : "Toggle" };
            if (adjustable(e))
                list[n++] = (Hint){ G_L2R2, e->kind == H5_LIGHT ? "Brightness" : e->kind == H5_CLIMATE ? "Temperature"
                                            : e->kind == H5_COVER ? "Position" : "Speed" };
            if (e->kind == H5_LIGHT || e->kind == H5_CLIMATE) list[n++] = (Hint){ G_TRIANGLE, "Details" };
            list[n++] = (Hint){ G_L1R1, "Rooms" };
            list[n++] = (Hint){ G_OPTIONS, "Menu" };
        } else {
            list[n++] = (Hint){ accept_glyph(ui), ui->nav < rooms ? "Open" : "Run" };
            list[n++] = (Hint){ G_L1R1, "Rooms" };
            list[n++] = (Hint){ G_OPTIONS, "Menu" };
        }
        hints(ui, c, MARGIN, 1012, list, n);
    }
    h5_free(ct);
}

static void light_screen(H5Ui *ui, H5Canvas *c)
{
    H5Entity *e = h5_ha_entity(&ui->ha, ui->detail_id);
    const H5Room *room = current_room(ui);
    char line[200];
    header(ui, c, room ? room->name : NULL);
    if (!e) return;
    uint32_t col = light_colour(e), lit = e->on ? col : 0xff3a4252u;
    float cx = 160 + 280, cy = 560;
    h5_circle(c, cx, cy, 280, (lit & 0x00ffffffu) | 0x1a000000u);
    h5_circle(c, cx, cy, 210, (lit & 0x00ffffffu) | 0x38000000u);
    h5_circle(c, cx, cy, 140, lit);
    H5Entity bulb = *e;
    bulb.kind = H5_LIGHT;
    icon(c, &bulb, cx - 60, cy - 60, 120, e->on ? 0xff3a2a12u : C_DIM);

    float x = 840;
    h5_text(c, &ui->fonts, H5_FACE_BIG, (int)x, 300, C_TEXT, entity_name(ui, e), 980, 0);
    snprintf(line, sizeof(line), "%s · %s", room ? room->name : "", e->on ? "On" : e->available ? "Off" : "Unavailable");
    h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)x, 346, C_DIM, line, 980, 0);
    if (e->brightness >= 0) {
        h5_text(c, &ui->fonts, H5_FACE_CAPS, (int)x, 418, C_FAINT, "BRIGHTNESS", 0, 0);
        snprintf(line, sizeof(line), "%d %%", e->on ? (e->brightness * 100 + 127) / 255 : 0);
        h5_text(c, &ui->fonts, H5_FACE_HUGE, (int)x - 4, 516, C_TEXT, line, 0, 0);
        bar(c, x, 548, 860, 14, e->on ? (float)e->brightness / 255 : 0, e->on ? col : C_DIM);
        int gx = (int)x;
        gx += pill_key(ui, c, "L2", gx, 606, 0) + 12;
        gx += h5_text(c, &ui->fonts, H5_FACE_BODY, gx, 614, C_DIM, "dims", 0, 0) + 28;
        gx += pill_key(ui, c, "R2", gx, 606, 0) + 12;
        h5_text(c, &ui->fonts, H5_FACE_BODY, gx, 614, C_DIM, "brightens · the triggers push back as you press", 0, 0);
    }
    Swatch list[8];
    int n = swatches(e, list);
    if (n) {
        h5_text(c, &ui->fonts, H5_FACE_CAPS, (int)x, 690, C_FAINT, "COLOUR", 0, 0);
        for (int i = 0; i < n; i++) {
            float sx = x + 38 + (float)i * 98;
            if (i == ui->swatch) h5_ring(c, sx, 750, 46, 4, C_FOCUS);
            h5_circle(c, sx, 750, 38, list[i].shown);
        }
        h5_text(c, &ui->fonts, H5_FACE_BODY, (int)x, 836, C_DIM, list[clampi(ui->swatch, 0, n - 1)].name, 0, 0);
    }
    /* the light bar setting */
    h5_round_rect(c, x, 868, 860, 96, 20, C_CARD);
    h5_text(c, &ui->fonts, H5_FACE_LABEL, (int)x + 32, 910, C_TEXT, "Light bar follows this light", 0, 0);
    h5_text(c, &ui->fonts, H5_FACE_SMALL, (int)x + 32, 942, C_DIM, "The controller glows in the light's colour", 0, 0);
    h5_round_rect(c, x + 860 - 116, 893, 84, 46, 23, ui->light_follow ? C_ACCENT : C_TRACK);
    h5_circle(c, ui->light_follow ? x + 860 - 55 : x + 860 - 93, 916, 18, 0xffffffffu);
    Hint hl[] = { { accept_glyph(ui), "On / off" }, { G_L2R2, "Brightness" }, { G_LR, "Colour" },
                  { G_SQUARE, "Light bar" }, { back_glyph(ui), "Back" } };
    int skip_colour = !n, skip_bright = e->brightness < 0;
    Hint shown[5];
    int count = 0;
    for (int i = 0; i < 5; i++)
        if (!((i == 2 && skip_colour) || (i == 1 && skip_bright))) shown[count++] = hl[i];
    hints(ui, c, MARGIN, 1012, shown, count);
}

static void climate_screen(H5Ui *ui, H5Canvas *c)
{
    H5Entity *e = h5_ha_entity(&ui->ha, ui->detail_id);
    const H5Room *room = current_room(ui);
    char line[200];
    header(ui, c, room ? room->name : NULL);
    if (!e) return;
    float cx = 460, cy = 560, r = 250;
    uint32_t col = !e->on ? C_DIM : !strcmp(e->state, "cool") ? C_ACCENT : C_ORANGE;
    h5_arc(c, cx, cy, r, 34, -2.356f, 2.356f, C_CARD_ON);
    if (!h5_isnan(e->target)) {
        float lo = h5_isnan(e->min_temp) ? 7 : e->min_temp, hi = h5_isnan(e->max_temp) ? 35 : e->max_temp;
        float f = hi > lo ? (e->target - lo) / (hi - lo) : 0.5f;
        f = fminf(fmaxf(f, 0), 1);
        h5_arc(c, cx, cy, r, 34, -2.356f, -2.356f + 4.712f * f, col);
        snprintf(line, sizeof(line), "%.1f°", e->target);
        h5_text(c, &ui->fonts, H5_FACE_CAPS, (int)cx, (int)cy - 90, C_FAINT, "TARGET", 0, 1);
        h5_text(c, &ui->fonts, H5_FACE_DIAL, (int)cx, (int)cy + 40, C_TEXT, line, 0, 1);
    }
    char action[64] = "";
    if (!h5_isnan(e->current)) snprintf(action, sizeof(action), "now %.1f %s", e->current, ui->ha.temp_unit);
    snprintf(line, sizeof(line), "%s%s%s", e->on ? (e->action[0] ? e->action : e->state) : "Off", action[0] ? " · " : "", action);
    if (line[0] >= 'a' && line[0] <= 'z') line[0] = (char)(line[0] - 32);
    h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)cx, (int)cy + 96, col == C_DIM ? C_DIM : col, line, 480, 1);

    float x = 860;
    h5_text(c, &ui->fonts, H5_FACE_BIG, (int)x, 300, C_TEXT, entity_name(ui, e), 960, 0);
    h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)x, 346, C_DIM, room ? room->name : "", 960, 0);
    for (int row = 0; row < 2; row++) {
        int count = row ? e->preset_count : e->mode_count;
        if (!count) continue;
        float y = row ? 650 : 440;
        h5_text(c, &ui->fonts, H5_FACE_CAPS, (int)x, (int)y, C_FAINT, row ? "PRESETS" : "MODE", 0, 0);
        float px = x;
        for (int i = 0; i < count; i++) {
            char label[H5_MODE];
            const char *value = row ? e->presets[i] : e->modes[i];
            h5_copy(label, sizeof(label), value);
            for (char *p = label; *p; p++)
                if (*p == '_') *p = ' ';
            if (!strcmp(label, "heat cool")) h5_copy(label, sizeof(label), "auto (heat/cool)");
            if (label[0] >= 'a' && label[0] <= 'z') label[0] = (char)(label[0] - 32);
            float w = (float)h5_text_width(&ui->fonts, H5_FACE_LABEL, label) + 72;
            if (px + w > W - MARGIN) break;
            int chosen = row ? !strcmp(e->preset, value) : !strcmp(e->state, value);
            int focus = ui->climate_row == row && ui->climate_col == i;
            if (focus) h5_round_rect(c, px - 4, y + 24, w + 8, 88, 44, C_FOCUS);
            h5_round_rect(c, px, y + 28, w, 80, 40, chosen ? col : C_CARD);
            h5_text(c, &ui->fonts, H5_FACE_LABEL, (int)(px + w / 2), (int)y + 78, chosen ? 0xff1a0f06u : C_TEXT, label, 0, 1);
            px += w + 16;
        }
    }
    Hint list[] = { { G_L2R2, "Temperature" }, { accept_glyph(ui), "Choose" }, { back_glyph(ui), "Back" } };
    hints(ui, c, MARGIN, 1012, list, 3);
}

/* The drawer on the right: the menu and the picker. */
static float drawer(H5Ui *ui, H5Canvas *c, const char *title)
{
    float x = W - 820;
    dim(c, 150);
    h5_shadow(c, x, -40, 880, H + 80, 0, 50, 0x90000000u);
    h5_round_rect(c, x, -40, 880, H + 80, 0, C_PANEL);
    h5_text(c, &ui->fonts, H5_FACE_HEAD, (int)x + 64, 140, C_TEXT, title, 820 - 128, 0);
    return x;
}

static void drawer_row(H5Ui *ui, H5Canvas *c, float x, float y, int focus, const char *label, const char *about,
                       const char *value, uint32_t value_col)
{
    int vw = value && value[0] ? h5_text_width(&ui->fonts, H5_FACE_LABEL, value) + 24 : 0;
    if (focus) h5_round_rect(c, x + 36, y, 820 - 72, 84, 16, 0xff1d2638u);
    if (focus) h5_round_rect(c, x + 36, y + 22, 5, 40, 2.5f, C_TEXT);
    h5_text(c, &ui->fonts, H5_FACE_LABEL, (int)x + 72, about && about[0] ? (int)y + 40 : (int)y + 52, C_TEXT, label,
            820 - 144 - vw, 0);
    if (about && about[0]) h5_text(c, &ui->fonts, H5_FACE_SMALL, (int)x + 72, (int)y + 70, C_DIM, about, 820 - 144 - vw, 0);
    if (vw) h5_text(c, &ui->fonts, H5_FACE_LABEL, (int)x + 820 - 72, (int)y + 52, value_col, value, 0, 2);
}

static void menu_screen(H5Ui *ui, H5Canvas *c)
{
    float x = drawer(ui, c, "Menu");
    for (int i = ui->menu_top; i < ui->menu_count && i < ui->menu_top + LIST_ROWS; i++) {
        char label[200], about[200], value[32];
        menu_text(ui, ui->menu[i], label, sizeof(label), about, sizeof(about), value, sizeof(value));
        drawer_row(ui, c, x, 190 + 88.0f * (float)(i - ui->menu_top), i == ui->menu_at, label, about, value,
                   i == ui->menu_at ? C_TEXT : C_ACCENT_TEXT);
    }
    Hint list[] = { { accept_glyph(ui), ui->menu[ui->menu_at] == M_LIGHT_FOLLOW ? "Change" : "Choose" },
                    { back_glyph(ui), "Back" } };
    hints(ui, c, (int)x + 64, 1012, list, 2);
}

static void pick_screen(H5Ui *ui, H5Canvas *c)
{
    const H5Room *room = current_room(ui);
    char title[120];
    snprintf(title, sizeof(title), "Add to %s", room ? room->name : "");
    float x = drawer(ui, c, title);
    h5_text(c, &ui->fonts, H5_FACE_SMALL, (int)x + 64, 178, C_DIM,
            "Every device in Home Assistant, by its room there. ✓ shows here.", 820 - 128, 0);
    for (int i = ui->pick_top; room && i < ui->pick_count && i < ui->pick_top + LIST_ROWS; i++) {
        const H5Entity *e = &ui->ha.entities[ui->pick[i]];
        const H5Area *area = h5_ha_area(&ui->ha, e->area);
        char about[200], status[96];
        h5_entity_status(e, ui->ha.temp_unit, status, sizeof(status));
        snprintf(about, sizeof(about), "%s%s%s", area ? area->name : "No room", status[0] ? " · " : "", status);
        drawer_row(ui, c, x, 210 + 88.0f * (float)(i - ui->pick_top), i == ui->pick_at, e->name, about,
                   in_room(ui, room, ui->pick[i]) ? "✓" : "", C_GREEN);
    }
    if (!ui->pick_count)
        h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)x + 72, 260, C_DIM, "No devices from Home Assistant yet.", 0, 0);
    Hint list[] = { { accept_glyph(ui), "Add / remove" }, { G_L1R1, "Page" }, { back_glyph(ui), "Done" } };
    hints(ui, c, (int)x + 64, 1012, list, 3);
}

static void confirm_screen(H5Ui *ui, H5Canvas *c)
{
    char question[200], detail[200] = "";
    const char *yes = "Yes";
    H5Entity *e = h5_ha_entity(&ui->ha, ui->confirm_id);
    switch (ui->confirm) {
    case C_LOCK:
        if (e && !strcmp(e->state, "locked")) {
            snprintf(question, sizeof(question), "Unlock %s?", entity_name(ui, e));
            yes = "Unlock";
        } else {
            snprintf(question, sizeof(question), "Lock %s?", e ? entity_name(ui, e) : "it");
            yes = "Lock";
        }
        break;
    case C_RESET:
        h5_copy(question, sizeof(question), "Reset the rooms?");
        h5_copy(detail, sizeof(detail), "Your added devices, names, rooms and order on this PS5 go. Home Assistant isn't touched.");
        yes = "Reset";
        break;
    case C_DELETE_ROOM:
        h5_copy(question, sizeof(question), "Delete this room?");
        h5_copy(detail, sizeof(detail), "Only the room on this PS5. Its devices stay in Home Assistant.");
        yes = "Delete";
        break;
    case C_SIGN_OUT:
        h5_copy(question, sizeof(question), "Sign out of Home Assistant?");
        h5_copy(detail, sizeof(detail), "The PS5 forgets the sign-in and Home Assistant revokes it.");
        yes = "Sign out";
        break;
    default:
        question[0] = 0;
    }
    dim(c, 170);
    float x = (W - 1000) / 2.0f, y = 380;
    h5_round_rect(c, x, y, 1000, 300, 28, C_PANEL);
    h5_text(c, &ui->fonts, H5_FACE_HEAD, (int)x + 72, (int)y + 110, C_TEXT, question, 856, 0);
    if (detail[0]) h5_text_wrap(c, &ui->fonts, H5_FACE_TEXT, (int)x + 72, (int)y + 164, 856, 2, 36, C_DIM, detail, 0);
    Hint list[] = { { accept_glyph(ui), yes }, { back_glyph(ui), "Cancel" } };
    hints(ui, c, (int)x + 72, (int)y + 260, list, 2);
}

static void about_screen(H5Ui *ui, H5Canvas *c)
{
    float x = (W - 1180) / 2.0f, y = 170;
    char line[400];
    dim(c, 170);
    h5_round_rect(c, x, y, 1180, 740, 28, C_PANEL);
    mark(c, x + 56, y + 40, 140, C_PANEL);
    h5_text(c, &ui->fonts, H5_FACE_TITLE, (int)x + 220, (int)y + 120, C_TEXT, "Home5", 0, 0);
    snprintf(line, sizeof(line), "Version %s · %s", H5_VERSION, H5_TITLE_ID);
    h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)x + 220, (int)y + 166, C_DIM, line, 0, 0);
    snprintf(line, sizeof(line), "Connected to %s (%s)", ui->ha.address,
             ui->ha.refresh[0] ? "signed in" : "long-lived token");
    h5_text(c, &ui->fonts, H5_FACE_TEXT, (int)x + 72, (int)y + 260, C_SOFT, ui->ha.address[0] ? line : "Not signed in", 1040, 0);
    h5_text_wrap(c, &ui->fonts, H5_FACE_BODY, (int)x + 72, (int)y + 330, 1040, 8, 36, C_DIM,
                 "Home Assistant on the PS5. Rooms, names and order you change here stay on this PS5; "
                 "Home Assistant is only sent the commands you give. Free software under the GNU GPL 3.0 or later, "
                 "built on the PS5 native app boilerplate, with mbedTLS (Apache 2.0), Mozilla's root certificates (MPL 2.0) "
                 "and the Inter font (SIL OFL 1.1). Source and licences: github.com/bornaradusin/Home5. "
                 "Home5 is not made by or connected to Home Assistant, the Open Home Foundation or Sony.", 0);
    Hint list[] = { { back_glyph(ui), "Back" } };
    hints(ui, c, (int)x + 72, (int)y + 690, list, 1);
}

void h5_ui_draw(H5Ui *ui, H5Canvas *canvas)
{
    if (!ui->fonts_ok) {
        h5_clear(canvas, C_BG);
        return;
    }
    h5_clear(canvas, C_BG);
    switch (ui->screen) {
    case S_SIGN_IN: sign_in_screen(ui, canvas); break;
    case S_CODE: code_screen(ui, canvas); break;
    case S_BUSY:
        busy_screen(ui, canvas);
        ui->busy_drawn = 1;
        break;
    case S_HOME: home_screen(ui, canvas); break;
    case S_LIGHT: light_screen(ui, canvas); break;
    case S_CLIMATE: climate_screen(ui, canvas); break;
    case S_MENU:
    case S_PICK:
        if (ui->menu_from == S_LIGHT && ui->screen == S_MENU) light_screen(ui, canvas);
        else if (ui->menu_from == S_CLIMATE && ui->screen == S_MENU) climate_screen(ui, canvas);
        else home_screen(ui, canvas);
        if (ui->screen == S_MENU) menu_screen(ui, canvas);
        else pick_screen(ui, canvas);
        break;
    case S_CONFIRM:
        if (ui->confirm == C_LOCK) home_screen(ui, canvas);
        else {
            home_screen(ui, canvas);
            menu_screen(ui, canvas);
        }
        confirm_screen(ui, canvas);
        break;
    case S_ABOUT:
        home_screen(ui, canvas);
        about_screen(ui, canvas);
        break;
    }
    toast_line(ui, canvas);
}

/* ---- the controller ------------------------------------------------------------------------------------------------ */

int32_t h5_ui_light_bar(const H5Ui *ui)
{
    H5Entity *e = focused((H5Ui *)ui);
    if (!ui->light_follow || !e || e->kind != H5_LIGHT || !e->on) return -1;
    return (int32_t)(light_colour(e) & 0x00ffffffu);
}

int h5_ui_triggers(const H5Ui *ui)
{
    return ui->ask == A_NONE && adjustable(focused((H5Ui *)ui));
}

/* ---- life ----------------------------------------------------------------------------------------------------------- */

static void ha_notify(void *context, const char *title, const char *message)
{
    H5Ui *ui = context;
    if (ui->platform.notify) ui->platform.notify(ui->platform.context, title, message);
}

H5Ui *h5_ui_create(const H5Platform *platform, const char *fonts_path)
{
    H5Ui *ui = h5_alloc(sizeof(H5Ui));
    if (!ui) return NULL;
    ui->platform = *platform;
    ui->fonts_ok = !h5_fonts_load(&ui->fonts, fonts_path);
    if (!ui->fonts_ok) say(ui, "H5 ui fonts missing: %s", fonts_path);
    h5_mkdirs(platform->data_dir);
    h5_join(ui->path_account, sizeof(ui->path_account), platform->data_dir, "account.txt");
    h5_join(ui->path_layout, sizeof(ui->path_layout), platform->data_dir, "layout.txt");
    h5_join(ui->path_settings, sizeof(ui->path_settings), platform->data_dir, "settings.txt");
    settings_load(ui);
    ui->layout = h5_alloc(sizeof(H5Layout));
    if (!ui->layout) {
        h5_free(ui);
        return NULL;
    }
    h5_layout_load(ui->layout, ui->path_layout);
    h5_ha_init(&ui->ha);
    ui->ha.clock_ms = platform->clock_ms;
    ui->ha.now = platform->unix_time;
    ui->ha.log = platform->log;
    ui->ha.notify = ha_notify;
    ui->ha.context = ui;
    ui->zone = Z_GRID;
    if (!h5_ha_load(&ui->ha, ui->path_account)) {
        h5_copy(ui->address, sizeof(ui->address), ui->ha.address);
        say(ui, "H5 ui account %s", ui->ha.address);
        start_busy(ui, B_CONNECT);
    } else {
        go(ui, S_SIGN_IN);
    }
    rebuild(ui);
    return ui;
}

void h5_ui_destroy(H5Ui *ui)
{
    if (!ui) return;
    h5_ha_free(&ui->ha);
    h5_view_free(&ui->view);
    h5_free(ui->pick);
    h5_free(ui->layout);
    h5_fonts_free(&ui->fonts);
    memset(ui->password, 0, sizeof(ui->password));
    h5_free(ui);
}

const char *h5_ui_screen(const H5Ui *ui) { return screen_names[ui->screen]; }
H5Ha *h5_ui_ha(H5Ui *ui) { return &ui->ha; }
const H5View *h5_ui_view(const H5Ui *ui) { return &ui->view; }
H5Layout *h5_ui_layout(H5Ui *ui) { return ui->layout; }
