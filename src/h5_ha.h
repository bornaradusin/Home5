/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: the Home Assistant client.
 *
 * Signing in uses HA's own login flow, as its phone app does: username and
 * password (and a two-factor code when the account has one) give a one-time
 * code, the code gives an access token and a refresh token. Only the refresh
 * token is kept (h5_ha_save); a long-lived access token from HA's profile page
 * works too. Then everything goes over HA's WebSocket API: the area, device
 * and entity registries, every entity's state, state changes as they happen,
 * persistent notifications, and service calls. Nothing on the server is
 * changed except by the service calls the player makes. */
#ifndef H5_HA_H
#define H5_HA_H
#include "h5.h"
#include "h5_net.h"

typedef enum H5Kind {
    /* cards, which turn on and off or are set */
    H5_LIGHT, H5_SWITCH, H5_FAN, H5_COVER, H5_CLIMATE, H5_LOCK, H5_MEDIA,
    /* actions, which run */
    H5_SCENE, H5_SCRIPT, H5_BUTTON,
    /* readings */
    H5_SENSOR, H5_BINARY,
    H5_KINDS
} H5Kind;

enum { H5_MODES = 8, H5_MODE = 24 };

typedef struct H5Entity {
    char id[128];          /* light.ceiling */
    char name[96];         /* what HA calls it */
    char area[64];         /* its area in HA: its own, else its device's ("" none) */
    char device[64];
    H5Kind kind;
    char state[48];        /* on, off, 21.4, heat, locked, unavailable */
    uint8_t available, on, hidden;   /* hidden: HA hides or disables it, or it's a config/diagnostic entity */
    /* lights */
    int16_t brightness;    /* 0-255; -1 when it doesn't dim */
    int32_t rgb;           /* 0xRRGGBB it shows, -1 unknown */
    uint8_t color, temp;   /* it takes colours, colour temperatures */
    int16_t kelvin, min_kelvin, max_kelvin;
    /* covers, fans */
    int8_t position;       /* 0-100, -1 when it can't be set */
    int8_t percentage;     /* fan speed 0-100, -1 none */
    /* climate */
    float current, target, step, min_temp, max_temp;   /* NAN when unknown */
    char modes[H5_MODES][H5_MODE];
    uint8_t mode_count;
    char presets[H5_MODES][H5_MODE];
    uint8_t preset_count;
    char preset[H5_MODE], action[H5_MODE];
    /* readings, media */
    char unit[16], device_class[32], media_title[96];
} H5Entity;

typedef struct H5Area {
    char id[64], name[64];
} H5Area;

typedef struct H5Device {
    char id[64], area[64];
} H5Device;

typedef struct H5Registered {
    char id[128], name[96], area[64], device[64];
    uint8_t hidden;
} H5Registered;

typedef enum H5Phase {
    H5_SIGNED_OUT,     /* no account */
    H5_OFFLINE,        /* an account, no connection (h5_ha_connect) */
    H5_LOADING,        /* connected, the registries and states on their way */
    H5_READY,
} H5Phase;

typedef struct H5Ha {
    /* the account */
    H5Url url;
    char address[300];          /* as the player typed it, tidied */
    char access[2048];          /* the access token (or a long-lived one) */
    char refresh[1024];         /* "" with a long-lived token */
    int64_t expires;            /* when access stops working (unix seconds), 0 never */
    char flow[96];              /* a login flow waiting for its two-factor code */
    /* the connection */
    H5Phase phase;
    H5Ws ws;
    uint32_t next_id;
    uint32_t want_areas, want_devices, want_entities, want_states, want_events, want_notes, want_config;
    int have;                   /* bits of what has come: 1 areas, 2 devices, 4 entities, 8 states */
    uint64_t last_sent_ms, last_heard_ms;
    /* the home */
    H5Area *areas;
    int area_count;
    H5Device *devices;
    int device_count;
    H5Registered *registered;
    int registered_count;
    H5Entity *entities;
    int count, cap;
    uint32_t version;           /* goes up with every change to the home */
    char temp_unit[8];          /* "°C" or "°F", HA's unit system */
    /* the platform */
    int64_t (*now)(void);       /* unix seconds */
    uint64_t (*clock_ms)(void);
    void (*notify)(void *context, const char *title, const char *message);
    void (*log)(const char *line);
    void *context;
    char error[H5_ERROR];       /* why the last thing failed, for the player */
} H5Ha;

void h5_ha_init(H5Ha *ha);
void h5_ha_free(H5Ha *ha);

/* ---- signing in (each blocks for a few seconds at most) -------------------------------- */
typedef enum H5Login { H5_LOGIN_FAILED = -1, H5_LOGIN_OK = 0, H5_LOGIN_CODE = 1 } H5Login;
/* address and username/password. H5_LOGIN_CODE: the account asks for a
 * two-factor code next (h5_ha_login_code). */
H5Login h5_ha_login(H5Ha *ha, const char *address, const char *username, const char *password);
H5Login h5_ha_login_code(H5Ha *ha, const char *code);
/* A long-lived access token instead. 0 or -1 (it's checked on the server). */
int h5_ha_use_token(H5Ha *ha, const char *address, const char *token);
/* A fresh access token from the refresh token, when it's due. 0 or -1. */
int h5_ha_refresh(H5Ha *ha);
/* Forget the account (and revoke its refresh token on the server, if it answers). */
void h5_ha_sign_out(H5Ha *ha);

/* The account in a file (address and refresh or long-lived token): 0 or -1. */
int h5_ha_save(const H5Ha *ha, const char *path);
int h5_ha_load(H5Ha *ha, const char *path);

/* ---- the live connection ---------------------------------------------------------------- */
/* Connects, signs the connection in and asks for everything. 0 (then
 * H5_LOADING), -1 (error says why; H5_SIGNED_OUT when the account was refused). */
int h5_ha_connect(H5Ha *ha);
/* Whatever has come, waiting at most timeout_ms for the first message: 1 when
 * the home changed, 0 when not, -1 when the connection broke (H5_OFFLINE). */
int h5_ha_poll(H5Ha *ha, int timeout_ms);
/* One message as if it came from HA (h5_ha_poll's work; the tests feed it). */
int h5_ha_feed(H5Ha *ha, const char *text, size_t length);
void h5_ha_disconnect(H5Ha *ha);
/* domain.service on entity, with data a JSON object's members ("\"brightness_pct\":50") or NULL. 0 or -1. */
int h5_ha_call(H5Ha *ha, const char *domain, const char *service, const char *entity, const char *data);

/* ---- the home -------------------------------------------------------------------------- */
H5Entity *h5_ha_entity(H5Ha *ha, const char *id);
const H5Area *h5_ha_area(const H5Ha *ha, const char *id);
const char *h5_kind_domain(H5Kind kind);
/* What a card shows under the name: "On · 80 %", "21.4 °C · Heating to 22.0 °C", "60 % open". */
void h5_entity_status(const H5Entity *e, const char *temp_unit, char *out, size_t size);

#endif
