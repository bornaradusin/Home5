/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: the rooms on this PS5. The player makes them and puts in each the
 * devices, scenes and sensors from Home Assistant they want there (a device
 * can be in more than one); they can rename devices and rooms and put the
 * rooms in their own order. Nothing comes in by itself from Home Assistant's
 * areas, and all of it is kept on the PS5 (<data>/layout.txt): Home
 * Assistant is never changed by it. */
#ifndef H5_LAYOUT_H
#define H5_LAYOUT_H
#include "h5_ha.h"

enum { H5_LAYOUT_ROOMS = 64, H5_LAYOUT_ITEMS = 1024, H5_LAYOUT_NAMES = 256 };

typedef struct H5Layout {
    struct { char id[64], name[64]; } rooms[H5_LAYOUT_ROOMS];     /* in the order shown; ids "ps5-N" */
    int room_count, next_room;
    struct { char room[64], entity[128]; } items[H5_LAYOUT_ITEMS];  /* what each room holds */
    int item_count;
    struct { char id[128], name[96]; } names[H5_LAYOUT_NAMES];     /* devices renamed here */
    int name_count;
} H5Layout;

typedef struct H5Room {
    char id[64];
    char name[64];
    int first, count;      /* what it holds that Home Assistant has: view->items[first .. first + count) */
} H5Room;

typedef struct H5View {
    H5Room *rooms;
    int room_count;
    int *items;            /* indices into ha->entities */
    int item_count;
} H5View;

/* No rooms. */
void h5_layout_reset(H5Layout *layout);
/* Reads 1.0.0's layout.txt too: its rooms made on the PS5 and what was added to them stay
 * (Home Assistant's areas were rooms then; now they aren't). 0, or -1 with the layout empty. */
int h5_layout_load(H5Layout *layout, const char *path);
int h5_layout_save(const H5Layout *layout, const char *path);

/* The rooms and what they hold that Home Assistant has: devices first (lights, then switches,
 * …), scenes and scripts after them, then sensors, by name within. */
void h5_layout_view(const H5Layout *layout, const H5Ha *ha, H5View *view);
void h5_view_free(H5View *view);
/* A device's name as this PS5 shows it, or fallback. */
const char *h5_layout_name(const H5Layout *layout, const char *id, const char *fallback);
/* A device's name in a room: renamed here, else HA's without the room's name
 * in front ("Living room Ceiling light" in the Living room: "Ceiling light"). */
void h5_display_name(const H5Layout *layout, const H5Entity *e, const char *room_name, char *out, size_t size);

/* Edits; each 0, or -1 when there's no room for it. */
int h5_layout_add(H5Layout *layout, const char *room, const char *entity);
void h5_layout_remove(H5Layout *layout, const char *room, const char *entity);
int h5_layout_has(const H5Layout *layout, const char *room, const char *entity);
/* A room's name (never empty), or a device's ("" puts HA's back). */
int h5_layout_rename(H5Layout *layout, const char *id, const char *name);
/* A new room, last; its id into id_out. */
int h5_layout_new_room(H5Layout *layout, const char *name, char *id_out, size_t size);
void h5_layout_delete_room(H5Layout *layout, const char *room);
/* Moves a room one place left (-1) or right (+1). */
void h5_layout_move_room(H5Layout *layout, const char *room, int step);

#endif
