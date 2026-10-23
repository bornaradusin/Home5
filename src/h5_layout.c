/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: the rooms as this PS5 shows them (h5_layout.h). */
#include "h5_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void h5_layout_reset(H5Layout *layout)
{
    memset(layout, 0, sizeof(*layout));
    layout->next_room = 1;
}

/* layout.txt, one setting a line:
 *   room = ps5-3 | Gaming corner
 *   order = living_room
 *   add = bedroom | light.hallway
 *   remove = kitchen | sensor.fridge_power
 *   name = light.ceiling_2 | Big light            */
int h5_layout_load(H5Layout *layout, const char *path)
{
    H5Buf text = { 0 };

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
        if (!strcmp(key, "room") && bar && layout->room_count < H5_LAYOUT_ROOMS) {
            h5_copy(layout->rooms[layout->room_count].id, 64, first);
            h5_copy(layout->rooms[layout->room_count].name, 64, second);
            layout->room_count++;
            int number = !strncmp(first, "ps5-", 4) ? atoi(first + 4) : 0;
            if (number >= layout->next_room) layout->next_room = number + 1;
        } else if (!strcmp(key, "order") && layout->order_count < H5_LAYOUT_ORDER) {
            h5_copy(layout->order[layout->order_count++], 64, first);
        } else if (!strcmp(key, "add") && bar && layout->add_count < H5_LAYOUT_ADDS) {
            h5_copy(layout->adds[layout->add_count].room, 64, first);
            h5_copy(layout->adds[layout->add_count].entity, 128, second);
            layout->add_count++;
        } else if (!strcmp(key, "remove") && bar && layout->remove_count < H5_LAYOUT_ADDS) {
            h5_copy(layout->removes[layout->remove_count].room, 64, first);
            h5_copy(layout->removes[layout->remove_count].entity, 128, second);
            layout->remove_count++;
        } else if (!strcmp(key, "name") && bar && layout->name_count < H5_LAYOUT_NAMES) {
            h5_copy(layout->names[layout->name_count].id, 128, first);
            h5_copy(layout->names[layout->name_count].name, 96, second);
            layout->name_count++;
        }
    }
    h5_buf_free(&text);
    return 0;
}

int h5_layout_save(const H5Layout *layout, const char *path)
{
    H5Buf text = { 0 };
    h5_buf_puts(&text, "# Home5's rooms on this PS5 (Home Assistant isn't changed by them)\n");
    for (int i = 0; i < layout->room_count; i++)
        h5_buf_printf(&text, "room = %s | %s\n", layout->rooms[i].id, layout->rooms[i].name);
    for (int i = 0; i < layout->order_count; i++) h5_buf_printf(&text, "order = %s\n", layout->order[i]);
    for (int i = 0; i < layout->add_count; i++)
        h5_buf_printf(&text, "add = %s | %s\n", layout->adds[i].room, layout->adds[i].entity);
    for (int i = 0; i < layout->remove_count; i++)
        h5_buf_printf(&text, "remove = %s | %s\n", layout->removes[i].room, layout->removes[i].entity);
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

static int removed(const H5Layout *layout, const char *room, const char *entity)
{
    for (int i = 0; i < layout->remove_count; i++)
        if (!strcmp(layout->removes[i].room, room) && !strcmp(layout->removes[i].entity, entity)) return 1;
    return 0;
}

int h5_layout_is_added(const H5Layout *layout, const char *room, const char *entity)
{
    for (int i = 0; i < layout->add_count; i++)
        if (!strcmp(layout->adds[i].room, room) && !strcmp(layout->adds[i].entity, entity)) return 1;
    return 0;
}

/* Lights first, then switches and the rest of the cards, actions, readings; by name within. */
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
    return h5_casecmp(na, nb);
}

typedef struct Candidate {
    char id[64], name[64];
    uint8_t custom, ordered;
    int order;
} Candidate;

static int compare_rooms(const void *pa, const void *pb)
{
    const Candidate *a = pa, *b = pb;
    if (a->ordered != b->ordered) return a->ordered ? -1 : 1;
    if (a->ordered) return a->order - b->order;
    /* Not placed by the player: HA's areas by name, then rooms made here, then "no area" last. */
    int ra = a->custom ? 1 : a->id[0] ? 0 : 2, rb = b->custom ? 1 : b->id[0] ? 0 : 2;
    if (ra != rb) return ra - rb;
    if (a->custom) return 0;
    return h5_casecmp(a->name, b->name);
}

void h5_layout_view(const H5Layout *layout, const H5Ha *ha, H5View *view)
{
    int total = ha->area_count + layout->room_count + 1;
    Candidate *rooms = h5_alloc(sizeof(Candidate) * (size_t)total);
    int *member = NULL, member_count = 0, member_cap = 0;

    memset(view, 0, sizeof(*view));
    if (!rooms) return;
    int n = 0;
    for (int i = 0; i < ha->area_count; i++, n++) {
        h5_copy(rooms[n].id, 64, ha->areas[i].id);
        h5_copy(rooms[n].name, 64, h5_layout_name(layout, ha->areas[i].id, ha->areas[i].name));
    }
    for (int i = 0; i < layout->room_count; i++, n++) {
        h5_copy(rooms[n].id, 64, layout->rooms[i].id);
        h5_copy(rooms[n].name, 64, layout->rooms[i].name);
        rooms[n].custom = 1;
    }
    h5_copy(rooms[n].id, 64, "");
    h5_copy(rooms[n].name, 64, h5_layout_name(layout, "", "Other devices"));
    n++;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < layout->order_count; k++)
            if (!strcmp(layout->order[k], rooms[i].id[0] ? rooms[i].id : "-")) {
                rooms[i].ordered = 1;
                rooms[i].order = k;
                break;
            }
    qsort(rooms, (size_t)n, sizeof(Candidate), compare_rooms);

    view->rooms = h5_alloc(sizeof(H5Room) * (size_t)n);
    if (!view->rooms) goto out;
    sort_ha = ha;
    sort_layout = layout;
    for (int r = 0; r < n; r++) {
        int first = member_count;
        for (int i = 0; i < ha->count; i++) {
            const H5Entity *e = &ha->entities[i];
            int in = (!e->hidden && !strcmp(e->area, rooms[r].id) && !rooms[r].custom &&
                      !removed(layout, rooms[r].id, e->id)) ||
                     h5_layout_is_added(layout, rooms[r].id, e->id);
            if (!in) continue;
            if (member_count == member_cap) {
                member_cap = member_cap ? member_cap * 2 : 256;
                int *grown = h5_realloc(member, sizeof(int) * (size_t)member_cap);
                if (!grown) goto out;
                member = grown;
            }
            member[member_count++] = i;
        }
        int count = member_count - first;
        /* An HA area with nothing to show stays out; a room made here stays, empty or not. */
        if (!count && !rooms[r].custom) continue;
        sort_room = rooms[r].name;
        if (count > 1) qsort(member + first, (size_t)count, sizeof(int), compare_items);
        H5Room *room = &view->rooms[view->room_count++];
        h5_copy(room->id, sizeof(room->id), rooms[r].id);
        h5_copy(room->name, sizeof(room->name), rooms[r].name);
        room->custom = rooms[r].custom;
        room->first = first;
        room->count = count;
    }
    view->items = member;
    view->item_count = member_count;
    member = NULL;
out:
    h5_free(member);
    h5_free(rooms);
}

void h5_view_free(H5View *view)
{
    h5_free(view->rooms);
    h5_free(view->items);
    memset(view, 0, sizeof(*view));
}

/* ---- edits -------------------------------------------------------------------------------------- */

int h5_layout_add(H5Layout *layout, const char *room, const char *entity)
{
    /* Taken out of HA's room before? Then it's just put back. */
    for (int i = 0; i < layout->remove_count; i++)
        if (!strcmp(layout->removes[i].room, room) && !strcmp(layout->removes[i].entity, entity)) {
            layout->removes[i] = layout->removes[--layout->remove_count];
            return 0;
        }
    if (h5_layout_is_added(layout, room, entity)) return 0;
    if (layout->add_count >= H5_LAYOUT_ADDS) return -1;
    h5_copy(layout->adds[layout->add_count].room, 64, room);
    h5_copy(layout->adds[layout->add_count].entity, 128, entity);
    layout->add_count++;
    return 0;
}

int h5_layout_remove(H5Layout *layout, const char *room, const char *entity, int added)
{
    if (added) {
        for (int i = 0; i < layout->add_count; i++)
            if (!strcmp(layout->adds[i].room, room) && !strcmp(layout->adds[i].entity, entity)) {
                memmove(&layout->adds[i], &layout->adds[i + 1], sizeof(layout->adds[0]) * (size_t)(layout->add_count - i - 1));
                layout->add_count--;
                return 0;
            }
        return 0;
    }
    if (removed(layout, room, entity)) return 0;
    if (layout->remove_count >= H5_LAYOUT_ADDS) return -1;
    h5_copy(layout->removes[layout->remove_count].room, 64, room);
    h5_copy(layout->removes[layout->remove_count].entity, 128, entity);
    layout->remove_count++;
    return 0;
}

int h5_layout_rename(H5Layout *layout, const char *id, const char *name)
{
    for (int i = 0; i < layout->room_count; i++)
        if (!strcmp(layout->rooms[i].id, id)) {
            if (name[0]) h5_copy(layout->rooms[i].name, 64, name);
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
    if (layout->room_count >= H5_LAYOUT_ROOMS) return -1;
    snprintf(layout->rooms[layout->room_count].id, 64, "ps5-%d", layout->next_room++);
    h5_copy(layout->rooms[layout->room_count].name, 64, name);
    h5_copy(id_out, size, layout->rooms[layout->room_count].id);
    layout->room_count++;
    return 0;
}

void h5_layout_delete_room(H5Layout *layout, const char *room)
{
    for (int i = 0; i < layout->room_count; i++)
        if (!strcmp(layout->rooms[i].id, room)) {
            memmove(&layout->rooms[i], &layout->rooms[i + 1], sizeof(layout->rooms[0]) * (size_t)(layout->room_count - i - 1));
            layout->room_count--;
            break;
        }
    for (int i = layout->add_count - 1; i >= 0; i--)
        if (!strcmp(layout->adds[i].room, room)) h5_layout_remove(layout, room, layout->adds[i].entity, 1);
    for (int i = layout->order_count - 1; i >= 0; i--)
        if (!strcmp(layout->order[i], room)) {
            memmove(&layout->order[i], &layout->order[i + 1], sizeof(layout->order[0]) * (size_t)(layout->order_count - i - 1));
            layout->order_count--;
        }
}

void h5_layout_move_room(H5Layout *layout, const H5View *view, const char *room, int step)
{
    int at = -1;
    for (int i = 0; i < view->room_count; i++)
        if (!strcmp(view->rooms[i].id, room)) at = i;
    int to = at + step;
    if (at < 0 || to < 0 || to >= view->room_count) return;
    /* The whole order as it shows now, with the two swapped: from then on it's the player's. */
    layout->order_count = 0;
    for (int i = 0; i < view->room_count && layout->order_count < H5_LAYOUT_ORDER; i++) {
        int k = i == at ? to : i == to ? at : i;
        h5_copy(layout->order[layout->order_count++], 64, view->rooms[k].id[0] ? view->rooms[k].id : "-");
    }
}
