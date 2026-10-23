/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: the rooms as this PS5 shows them. They start as Home Assistant's
 * areas with the devices HA puts in them; on top of that the player can add a
 * device to any room (it stays where HA has it too), remove one from a room,
 * rename devices and rooms, make rooms of their own, and put rooms in another
 * order. All of that is kept on the PS5 (<data>/layout.txt); Home Assistant
 * is never changed by it. */
#ifndef H5_LAYOUT_H
#define H5_LAYOUT_H
#include "h5_ha.h"

enum { H5_LAYOUT_ROOMS = 64, H5_LAYOUT_ORDER = 128, H5_LAYOUT_ADDS = 512, H5_LAYOUT_NAMES = 256 };

typedef struct H5Layout {
    struct { char id[64], name[64]; } rooms[H5_LAYOUT_ROOMS];   /* made on this PS5 ("ps5-3") */
    int room_count, next_room;
    char order[H5_LAYOUT_ORDER][64];                              /* rooms as the player put them */
    int order_count;
    struct { char room[64], entity[128]; } adds[H5_LAYOUT_ADDS];  /* added to a room */
    int add_count;
    struct { char room[64], entity[128]; } removes[H5_LAYOUT_ADDS];   /* taken out of HA's room */
    int remove_count;
    struct { char id[128], name[96]; } names[H5_LAYOUT_NAMES];   /* devices and rooms, renamed here */
    int name_count;
} H5Layout;

typedef struct H5Room {
    char id[64];           /* HA's area id, "ps5-N" for one made here, "" for devices in no area */
    char name[64];
    uint8_t custom;        /* made on this PS5 */
    int first, count;      /* its devices: view->items[first .. first + count) */
} H5Room;

typedef struct H5View {
    H5Room *rooms;
    int room_count;
    int *items;            /* indices into ha->entities */
    int item_count;
} H5View;

/* An empty layout (all of Home Assistant's, as it is). */
void h5_layout_reset(H5Layout *layout);
int h5_layout_load(H5Layout *layout, const char *path);    /* 0, or -1 with the layout empty */
int h5_layout_save(const H5Layout *layout, const char *path);

/* The rooms and their devices, Home Assistant's with the layout over them. */
void h5_layout_view(const H5Layout *layout, const H5Ha *ha, H5View *view);
void h5_view_free(H5View *view);
/* A device's or a room's name as this PS5 shows it. */
const char *h5_layout_name(const H5Layout *layout, const char *id, const char *fallback);
/* A device's name in a room: renamed here, else HA's without the room's name
 * in front ("Living room Ceiling light" in the Living room: "Ceiling light"). */
void h5_display_name(const H5Layout *layout, const H5Entity *e, const char *room_name, char *out, size_t size);

/* Edits; each 0, or -1 when there's no room for it. */
int h5_layout_add(H5Layout *layout, const char *room, const char *entity);
/* Out of room: an added device is un-added, one HA put there is taken out. */
int h5_layout_remove(H5Layout *layout, const char *room, const char *entity, int added);
int h5_layout_is_added(const H5Layout *layout, const char *room, const char *entity);
int h5_layout_rename(H5Layout *layout, const char *id, const char *name);   /* "" puts HA's name back */
/* A new room's id into id_out. */
int h5_layout_new_room(H5Layout *layout, const char *name, char *id_out, size_t size);
void h5_layout_delete_room(H5Layout *layout, const char *room);
/* Moves room up (-1) or down (+1) among the rooms of view. */
void h5_layout_move_room(H5Layout *layout, const H5View *view, const char *room, int step);

#endif
