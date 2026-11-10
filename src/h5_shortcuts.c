/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: controller shortcuts (h5_shortcuts.h). */
#include "h5_shortcuts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The buttons in the order a combo and the file show them. */
static const struct { uint32_t mask; const char *name; } BTNS[] = {
    { H5BTN_L1, "L1" }, { H5BTN_R1, "R1" }, { H5BTN_L2, "L2" }, { H5BTN_R2, "R2" },
    { H5BTN_L3, "L3" }, { H5BTN_R3, "R3" },
    { H5BTN_UP, "UP" }, { H5BTN_DOWN, "DOWN" }, { H5BTN_LEFT, "LEFT" }, { H5BTN_RIGHT, "RIGHT" },
    { H5BTN_TRIANGLE, "TRIANGLE" }, { H5BTN_CIRCLE, "CIRCLE" }, { H5BTN_CROSS, "CROSS" }, { H5BTN_SQUARE, "SQUARE" },
    { H5BTN_OPTIONS, "OPTIONS" }, { H5BTN_TOUCH, "TOUCH" },
};
enum { BTN_COUNT = sizeof(BTNS) / sizeof(BTNS[0]) };

int h5_btn_count(uint32_t mask)
{
    int n = 0;
    for (int i = 0; i < BTN_COUNT; i++) n += (mask & BTNS[i].mask) != 0;
    return n;
}

const char *h5_btn_name(uint32_t one)
{
    for (int i = 0; i < BTN_COUNT; i++)
        if (one == BTNS[i].mask) return BTNS[i].name;
    return "";
}

static uint32_t btn_from_name(const char *name, size_t n)
{
    for (int i = 0; i < BTN_COUNT; i++)
        if (strlen(BTNS[i].name) == n && !strncmp(BTNS[i].name, name, n)) return BTNS[i].mask;
    return 0;
}

int h5_combo_buttons(uint32_t combo, uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < BTN_COUNT && n < max; i++)
        if (combo & BTNS[i].mask) out[n++] = BTNS[i].mask;
    return n;
}

void h5_shortcuts_reset(H5Shortcuts *s)
{
    memset(s, 0, sizeof(*s));
    s->enabled = 1;
    s->rumble_str = 180;
    s->rumble_ms = 350;
}

/* name|entity|domain|service|data|label|mode|confirm|trigger into up to 9 trimmed parts. */
static int split9(char *value, char *parts[9])
{
    int n = 0;
    parts[n++] = value;
    for (char *p = value; *p && n < 9; p++)
        if (p[0] == '|') {
            *p = 0;
            parts[n++] = p + 1;
        }
    for (int i = n; i < 9; i++) parts[i] = (char *)"";
    for (int i = 0; i < 9; i++) parts[i] = h5_trim(parts[i]);
    return n;
}

static const char *confirm_name(uint8_t confirm)
{
    return confirm == H5_FB_NOTIFY ? "notify" : confirm == H5_FB_RUMBLE ? "rumble" : confirm == H5_FB_ALL ? "all" : "bar";
}

static uint8_t confirm_from(const char *name)
{
    return !strcmp(name, "notify") ? H5_FB_NOTIFY : !strcmp(name, "rumble") ? H5_FB_RUMBLE
           : !strcmp(name, "all") || !strcmp(name, "both") ? H5_FB_ALL : H5_FB_LIGHTBAR;
}

static void parse_trigger(H5Shortcut *sc, const char *mode, char *trigger)
{
    sc->mode = !strcmp(mode, "seq") ? H5_TRIG_SEQUENCE : H5_TRIG_COMBO;
    sc->combo = 0;
    sc->len = 0;
    const char sep = sc->mode == H5_TRIG_SEQUENCE ? '>' : '+';
    for (char *p = trigger; *p;) {
        size_t n = 0;
        while (p[n] && p[n] != sep) n++;
        uint32_t b = btn_from_name(p, n);
        if (b) {
            if (sc->mode == H5_TRIG_SEQUENCE) {
                if (sc->len < H5_SEQ_MAX) sc->seq[sc->len++] = b;
            } else {
                sc->combo |= b;
            }
        }
        p += n + (p[n] == sep);
    }
    if (sc->mode == H5_TRIG_COMBO) sc->len = (uint8_t)h5_btn_count(sc->combo);
}

int h5_shortcuts_load(H5Shortcuts *s, const char *path)
{
    H5Buf text = { 0 };
    h5_shortcuts_reset(s);
    if (h5_read_file(path, &text, 256 * 1024)) return -1;
    for (char *line = (char *)text.data, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *key = h5_trim(line), *value = h5_trim(eq + 1);
        if (!strcmp(key, "enabled")) {
            s->enabled = strcmp(value, "off") != 0;
        } else if (!strcmp(key, "rumble_str")) {
            int v = atoi(value);
            if (v >= 0 && v <= 255) s->rumble_str = (uint16_t)v;
        } else if (!strcmp(key, "rumble_ms")) {
            int v = atoi(value);
            if (v >= 50 && v <= 3000) s->rumble_ms = (uint16_t)v;
        } else if (!strcmp(key, "shortcut") && s->count < H5_SHORTCUTS_MAX) {
            char *p[9];
            split9(value, p);
            if (!p[0][0] || !p[1][0]) continue;   /* needs a name and an entity */
            H5Shortcut *sc = &s->items[s->count];
            memset(sc, 0, sizeof(*sc));
            h5_copy(sc->name, sizeof(sc->name), p[0]);
            h5_copy(sc->entity, sizeof(sc->entity), p[1]);
            h5_copy(sc->domain, sizeof(sc->domain), p[2]);
            h5_copy(sc->service, sizeof(sc->service), p[3]);
            h5_copy(sc->data, sizeof(sc->data), p[4]);
            h5_copy(sc->label, sizeof(sc->label), p[5]);
            sc->confirm = confirm_from(p[7]);
            parse_trigger(sc, p[6], p[8]);
            s->count++;
        }
    }
    h5_buf_free(&text);
    return 0;
}

static void trigger_text(const H5Shortcut *sc, char *out, size_t size)
{
    out[0] = 0;
    if (sc->mode == H5_TRIG_SEQUENCE) {
        for (int i = 0; i < sc->len; i++) {
            if (i) strncat(out, ">", size - strlen(out) - 1);
            strncat(out, h5_btn_name(sc->seq[i]), size - strlen(out) - 1);
        }
    } else {
        uint32_t keys[H5_SEQ_MAX];
        int n = h5_combo_buttons(sc->combo, keys, H5_SEQ_MAX);
        for (int i = 0; i < n; i++) {
            if (i) strncat(out, "+", size - strlen(out) - 1);
            strncat(out, h5_btn_name(keys[i]), size - strlen(out) - 1);
        }
    }
}

int h5_shortcuts_save(const H5Shortcuts *s, const char *path)
{
    H5Buf text = { 0 };
    h5_buf_puts(&text, "# Home5 controller shortcuts\nversion = 1\n");
    h5_buf_printf(&text, "enabled = %s\n", s->enabled ? "on" : "off");
    h5_buf_printf(&text, "rumble_str = %d\n", s->rumble_str);
    h5_buf_printf(&text, "rumble_ms = %d\n", s->rumble_ms);
    for (int i = 0; i < s->count; i++) {
        const H5Shortcut *sc = &s->items[i];
        char trigger[128];
        trigger_text(sc, trigger, sizeof(trigger));
        h5_buf_printf(&text, "shortcut = %s | %s | %s | %s | %s | %s | %s | %s | %s\n", sc->name, sc->entity, sc->domain,
                      sc->service, sc->data, sc->label, sc->mode == H5_TRIG_SEQUENCE ? "seq" : "combo",
                      confirm_name(sc->confirm), trigger);
    }
    int ok = text.data ? h5_write_file(path, text.data, text.len) : -1;
    h5_buf_free(&text);
    return ok;
}

int h5_shortcuts_add(H5Shortcuts *s, const char *name, const char *entity, const char *domain, const char *service,
                     const char *data, const char *label)
{
    if (s->count >= H5_SHORTCUTS_MAX) return -1;
    H5Shortcut *sc = &s->items[s->count];
    memset(sc, 0, sizeof(*sc));
    h5_copy(sc->name, sizeof(sc->name), name);
    h5_copy(sc->entity, sizeof(sc->entity), entity);
    h5_copy(sc->domain, sizeof(sc->domain), domain);
    h5_copy(sc->service, sizeof(sc->service), service);
    h5_copy(sc->data, sizeof(sc->data), data ? data : "");
    h5_copy(sc->label, sizeof(sc->label), label);
    sc->mode = H5_TRIG_COMBO;
    sc->confirm = H5_FB_LIGHTBAR;
    return s->count++;
}

void h5_shortcuts_delete(H5Shortcuts *s, int index)
{
    if (index < 0 || index >= s->count) return;
    memmove(&s->items[index], &s->items[index + 1], sizeof(s->items[0]) * (size_t)(s->count - index - 1));
    s->count--;
}

/* ---- the reader's config ---------------------------------------------------------------------- */

/* The live copy the reader thread reads, straight on the system tmpfs so it survives a game
 * unmounting /data from the app's sandbox (SceShellUI can't read /data anyway). */
#define READER_TMP "/system_tmp/home5_reader.cfg"

static int reader_path(char *out, size_t size, const char *dir)
{
    return h5_join(out, size, dir, "shortcuts.cfg");
}

int h5_shortcuts_write_reader(const H5Shortcuts *s, const char *dir, const char *url, const char *access,
                              const char *refresh, int64_t expires, const char *client)
{
    char path[H5_PATH];
    H5Buf text = { 0 };
    int ready = 0;
    if (!s->enabled || !access || !access[0] || !url || !url[0]) {
        h5_shortcuts_clear_reader(dir);
        return 0;
    }
    if (h5_mkdirs(dir) || reader_path(path, sizeof(path), dir)) return -1;
    h5_buf_puts(&text, "# Home5 background shortcuts — generated by the app, do not edit\n");
    h5_buf_printf(&text, "url = %s\n", url);
    h5_buf_printf(&text, "access = %s\n", access);
    h5_buf_printf(&text, "refresh = %s\n", refresh ? refresh : "");
    h5_buf_printf(&text, "expires = %lld\n", (long long)expires);
    h5_buf_printf(&text, "client = %s\n", client ? client : "");
    h5_buf_puts(&text, "hold_ms = 150\n");
    h5_buf_printf(&text, "rumble_strength = %d\n", s->rumble_str);
    h5_buf_printf(&text, "rumble_ms = %d\n", s->rumble_ms);
    h5_buf_puts(&text, "# sc = mode | confirm | domain | service | entity | data | keys | name\n");
    for (int i = 0; i < s->count; i++) {
        const H5Shortcut *sc = &s->items[i];
        char keys[128] = "";
        if (sc->mode == H5_TRIG_SEQUENCE) {
            for (int k = 0; k < sc->len; k++) {
                char one[12];
                snprintf(one, sizeof(one), "%s0x%x", k ? "," : "", sc->seq[k]);
                strncat(keys, one, sizeof(keys) - strlen(keys) - 1);
            }
        } else {
            snprintf(keys, sizeof(keys), "0x%x", sc->combo);
        }
        if (!sc->domain[0] || !sc->service[0] || sc->len == 0) continue;   /* not finished */
        h5_buf_printf(&text, "sc = %s | %s | %s | %s | %s | %s | %s | %s\n",
                      sc->mode == H5_TRIG_SEQUENCE ? "seq" : "combo", confirm_name(sc->confirm), sc->domain, sc->service,
                      sc->entity, sc->data, keys, sc->name);
        ready++;
    }
    int ok = -1;
    if (ready && text.data) {
        int ok_data = h5_write_file(path, text.data, text.len);
        /* Also write it live to /system_tmp, where the reader thread reads it, so an edit made while
         * a game has /data unmounted still reaches the reader without restarting the app. */
        int ok_tmp = h5_write_file(READER_TMP, text.data, text.len);
        ok = (ok_data == 0 || ok_tmp == 0) ? 0 : -1;
    } else {
        h5_shortcuts_clear_reader(dir);   /* nothing finished: no config */
    }
    memset(text.data, 0, text.cap);   /* the file held a token */
    h5_buf_free(&text);
    return ready ? ok : 0;
}

void h5_shortcuts_clear_reader(const char *dir)
{
    char path[H5_PATH];
    if (!reader_path(path, sizeof(path), dir)) remove(path);
    remove(READER_TMP);
}
