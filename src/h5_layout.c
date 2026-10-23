/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: the rooms on this PS5 (h5_layout.h). */
#include "h5_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void h5_layout_reset(H5Layout *layout)
{
    memset(layout, 0, sizeof(*layout));
    layout->next_room = 1;
}

static int room_at(const H5Layout *layout, const char *room)
{
    for (int i = 0; i < layout->room_count; i++)
        if (!strcmp(layout->rooms[i].id, room)) return i;
    return -1;
}

/* layout.txt, one setting a line:
 *   version = 2
 *   room = ps5-3 | Gaming corner        (in the order shown)
 *   item = ps5-3 | light.desk_lamp
 *   name = light.ceiling_2 | Big light
 * 1.0.0's had "add = <room> | <entity>" for the same as item, "order = <room>" for the order, and
 * Home Assistant's areas among the rooms: those, and "remove" lines, are left out. */
int h5_layout_load(H5Layout *layout, const char *path)
{
    H5Buf text = { 0 };
    static char order[H5_LAYOUT_ROOMS][64];
    int order_count = 0;

    h5_layout_reset(layout);
    if (h5_read_file(path, &text, 1024 * 1024)) return -1;
    for (char *line = (char *)text.data, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *key = h5_trim(line), *value = h5_trim(eq + 1), *bar = strchr(value, '|');
        char *first = value, *second = "";
        if (bar) {
            *bar = 0;
            first = h5_trim(value);
            second = h5_trim(bar + 1);
        }
        if (!strcmp(key, "room") && bar && first[0] && second[0] && layout->room_count < H5_LAYOUT_ROOMS &&
            room_at(layout, first) < 0) {
            h5_copy(layout->rooms[layout->room_count].id, 64, first);
            h5_copy(layout->rooms[layout->room_count].name, 64, second);
            layout->room_count++;
            int number = !strncmp(first, "ps5-", 4) ? atoi(first + 4) : 0;
            if (number >= layout->next_room) layout->next_room = number + 1;
        } else if ((!strcmp(key, "item") || !strcmp(key, "add")) && bar && second[0] &&
                   layout->item_count < H5_LAYOUT_ITEMS && !h5_layout_has(layout, first, second)) {
            h5_copy(layout->items[layout->item_count].room, 64, first);
            h5_copy(layout->items[layout->item_count].entity, 128, second);
            layout->item_count++;
        } else if (!strcmp(key, "name") && bar && strchr(first, '.') && second[0] && layout->name_count < H5_LAYOUT_NAMES) {
            h5_copy(layout->names[layout->name_count].id, 128, first);
            h5_copy(layout->names[layout->name_count].name, 96, second);
            layout->name_count++;
        } else if (!strcmp(key, "order") && order_count < H5_LAYOUT_ROOMS) {
            h5_copy(order[order_count++], 64, first);
        }
    }
    h5_buf_free(&text);
    /* What belongs to no room (1.0.0's additions to Home Assistant's areas) goes. */
    int kept = 0;
    for (int i = 0; i < layout->item_count; i++)
        if (room_at(layout, layout->items[i].room) >= 0) layout->items[kept++] = layout->items[i];
    layout->item_count = kept;
    /* 1.0.0's order, among the rooms that are left. */
    for (int k = order_count - 1; k >= 0; k--) {
        int at = room_at(layout, order[k]);
        if (at <= 0) continue;
        char id[64], name[64];
        h5_copy(id, sizeof(id), layout->rooms[at].id);
        h5_copy(name, sizeof(name), layout->rooms[at].name);
        memmove(&layout->rooms[1], &layout->rooms[0], sizeof(layout->rooms[0]) * (size_t)at);
        h5_copy(layout->rooms[0].id, 64, id);
        h5_copy(layout->rooms[0].name, 64, name);
    }
    return 0;
}

int h5_layout_save(const H5Layout *layout, const char *path)
{
    H5Buf text = { 0 };
    h5_buf_puts(&text, "# Home5's rooms on this PS5 (Home Assistant isn't changed by them)\nversion = 2\n");
    for (int i = 0; i < layout->room_count; i++)
        h5_buf_printf(&text, "room = %s | %s\n", layout->rooms[i].id, layout->rooms[i].name);
    for (int i = 0; i < layout->item_count; i++)
        h5_buf_printf(&text, "item = %s | %s\n", layout->items[i].room, layout->items[i].entity);
    for (int i = 0; i < layout->name_count; i++)
        h5_buf_printf(&text, "name = %s | %s\n", layout->names[i].id, layout->names[i].name);
    int ok = text.data ? h5_write_file(path, text.data, text.len) : -1;
    h5_buf_free(&text);
    return ok;
}

const char *h5_layout_name(const H5Layout *layout, const char *id, const char *fallback)
{
    for (int i = 0; i < layout->name_count; i++)
        if (!strcmp(layout->names[i].id, id)) return layout->names[i].name;
    return fallback;
}

void h5_display_name(const H5Layout *layout, const H5Entity *e, const char *room_name, char *out, size_t size)
{
    const char *renamed = h5_layout_name(layout, e->id, NULL);
    size_t n = room_name ? strlen(room_name) : 0;
    if (renamed) {
        h5_copy(out, size, renamed);
    } else if (n && strlen(e->name) > n + 1 && e->name[n] == ' ' && !h5_casecmp_n(e->name, room_name, n)) {
        h5_copy(out, size, e->name + n + 1);
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
    } else {
        h5_copy(out, size, e->name);
    }
}

/* ---- the view ---------------------------------------------------------------------------------- */

/* Lights first, then switches and the rest of the devices, scenes and scripts, sensors; by name within. */
static const H5Ha *sort_ha;
static const H5Layout *sort_layout;
static const char *sort_room;

static int compare_items(const void *pa, const void *pb)
{
    const H5Entity *a = &sort_ha->entities[*(const int *)pa], *b = &sort_ha->entities[*(const int *)pb];
    char na[96], nb[96];
    if (a->kind != b->kind) return (int)a->kind - (int)b->kind;
    h5_display_name(sort_layout, a, sort_room, na, sizeof(na));
    h5_display_name(sort_layout, b, sort_room, nb, sizeof(nb));
    int c = h5_casecmp(na, nb);
    return c ? c : strcmp(a->id, b->id);
}

/* Home Assistant's entities by id, for finding a room's quickly (this runs on every state change). */
typedef struct ById { const char *id; int index; } ById;

static int compare_by_id(const void *pa, const void *pb)
{
    return strcmp(((const ById *)pa)->id, ((const ById *)pb)->id);
}

void h5_layout_view(const H5Layout *layout, const H5Ha *ha, H5View *view)
{
    ById *by_id = h5_alloc(sizeof(ById) * (size_t)(ha->count ? ha->count : 1));
    memset(view, 0, sizeof(*view));
    view->rooms = h5_alloc(sizeof(H5Room) * (size_t)(layout->room_count ? layout->room_count : 1));
    view->items = h5_alloc(sizeof(int) * (size_t)(layout->item_count ? layout->item_count : 1));
    if (!view->rooms || !view->items || !by_id) {
        h5_view_free(view);
        h5_free(by_id);
        return;
    }
    for (int i = 0; i < ha->count; i++) by_id[i] = (ById){ ha->entities[i].id, i };
    if (ha->count > 1) qsort(by_id, (size_t)ha->count, sizeof(ById), compare_by_id);
    sort_ha = ha;
    sort_layout = layout;
    for (int r = 0; r < layout->room_count; r++) {
        H5Room *room = &view->rooms[view->room_count++];
        h5_copy(room->id, sizeof(room->id), layout->rooms[r].id);
        h5_copy(room->name, sizeof(room->name), layout->rooms[r].name);
        room->first = view->item_count;
        /* What Home Assistant has now: one it no longer has stays in the file, unshown. */
        for (int i = 0; i < layout->item_count; i++) {
            if (strcmp(layout->items[i].room, room->id)) continue;
            ById key = { layout->items[i].entity, 0 }, *found;
            found = ha->count ? bsearch(&key, by_id, (size_t)ha->count, sizeof(ById), compare_by_id) : NULL;
            if (found) view->items[view->item_count++] = found->index;
        }
        room->count = view->item_count - room->first;
        sort_room = room->name;
        if (room->count > 1) qsort(view->items + room->first, (size_t)room->count, sizeof(int), compare_items);
    }
    h5_free(by_id);
}

void h5_view_free(H5View *view)
{
    h5_free(view->rooms);
    h5_free(view->items);
    memset(view, 0, sizeof(*view));
}

/* ---- edits -------------------------------------------------------------------------------------- */

int h5_layout_has(const H5Layout *layout, const char *room, const char *entity)
{
    for (int i = 0; i < layout->item_count; i++)
        if (!strcmp(layout->items[i].room, room) && !strcmp(layout->items[i].entity, entity)) return 1;
    return 0;
}

int h5_layout_add(H5Layout *layout, const char *room, const char *entity)
{
    if (h5_layout_has(layout, room, entity)) return 0;
    if (room_at(layout, room) < 0 || layout->item_count >= H5_LAYOUT_ITEMS) return -1;
    h5_copy(layout->items[layout->item_count].room, 64, room);
    h5_copy(layout->items[layout->item_count].entity, 128, entity);
    layout->item_count++;
    return 0;
}

void h5_layout_remove(H5Layout *layout, const char *room, const char *entity)
{
    for (int i = 0; i < layout->item_count; i++)
        if (!strcmp(layout->items[i].room, room) && !strcmp(layout->items[i].entity, entity)) {
            memmove(&layout->items[i], &layout->items[i + 1], sizeof(layout->items[0]) * (size_t)(layout->item_count - i - 1));
            layout->item_count--;
            return;
        }
}

int h5_layout_rename(H5Layout *layout, const char *id, const char *name)
{
    int room = room_at(layout, id);
    if (room >= 0) {
        if (name[0]) h5_copy(layout->rooms[room].name, 64, name);
        return 0;
    }
    for (int i = 0; i < layout->name_count; i++)
        if (!strcmp(layout->names[i].id, id)) {
            if (name[0]) h5_copy(layout->names[i].name, 96, name);
            else layout->names[i] = layout->names[--layout->name_count];
            return 0;
        }
    if (!name[0]) return 0;
    if (layout->name_count >= H5_LAYOUT_NAMES) return -1;
    h5_copy(layout->names[layout->name_count].id, 128, id);
    h5_copy(layout->names[layout->name_count].name, 96, name);
    layout->name_count++;
    return 0;
}

int h5_layout_new_room(H5Layout *layout, const char *name, char *id_out, size_t size)
{
    if (layout->room_count >= H5_LAYOUT_ROOMS || !name[0]) return -1;
    snprintf(layout->rooms[layout->room_count].id, 64, "ps5-%d", layout->next_room++);
    h5_copy(layout->rooms[layout->room_count].name, 64, name);
    h5_copy(id_out, size, layout->rooms[layout->room_count].id);
    layout->room_count++;
    return 0;
}

void h5_layout_delete_room(H5Layout *layout, const char *room)
{
    int at = room_at(layout, room);
    if (at < 0) return;
    memmove(&layout->rooms[at], &layout->rooms[at + 1], sizeof(layout->rooms[0]) * (size_t)(layout->room_count - at - 1));
    layout->room_count--;
    int kept = 0;
    for (int i = 0; i < layout->item_count; i++)
        if (strcmp(layout->items[i].room, room)) layout->items[kept++] = layout->items[i];
    layout->item_count = kept;
}

void h5_layout_move_room(H5Layout *layout, const char *room, int step)
{
    int at = room_at(layout, room), to = at + step;
    if (at < 0 || to < 0 || to >= layout->room_count) return;
    char id[64], name[64];
    h5_copy(id, sizeof(id), layout->rooms[at].id);
    h5_copy(name, sizeof(name), layout->rooms[at].name);
    layout->rooms[at] = layout->rooms[to];
    h5_copy(layout->rooms[to].id, 64, id);
    h5_copy(layout->rooms[to].name, 64, name);
}
