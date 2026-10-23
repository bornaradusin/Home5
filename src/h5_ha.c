/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: the Home Assistant client (h5_ha.h). */
#include "h5_ha.h"
#include "h5_json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HA's login flow wants the app to name itself by a web address, and to come
 * back to one on the same host (IndieAuth): Home5's repository. */
#define CLIENT_ID H5_REPO
#define REDIRECT_URI H5_REPO "/signed-in"

enum {
    HAVE_AREAS = 1, HAVE_DEVICES = 2, HAVE_ENTITIES = 4, HAVE_STATES = 8, HAVE_ALL = 15,
    PING_EVERY_MS = 30000,
    SILENT_MS = 75000,        /* nothing heard for this long: the connection is gone */
    AUTH_WAIT_MS = 10000,
};

static void say(H5Ha *ha, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void say(H5Ha *ha, const char *format, ...)
{
    char line[700];
    va_list args;
    if (!ha->log) return;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ha->log(line);
}

static uint64_t clock_ms(H5Ha *ha) { return ha->clock_ms ? ha->clock_ms() : 0; }
static int64_t now_s(H5Ha *ha) { return ha->now ? ha->now() : 0; }

void h5_ha_init(H5Ha *ha)
{
    memset(ha, 0, sizeof(*ha));
    ha->phase = H5_SIGNED_OUT;
    h5_copy(ha->temp_unit, sizeof(ha->temp_unit), "°C");
}

static void forget_home(H5Ha *ha)
{
    h5_free(ha->areas);
    h5_free(ha->devices);
    h5_free(ha->registered);
    h5_free(ha->entities);
    ha->areas = NULL;
    ha->devices = NULL;
    ha->registered = NULL;
    ha->entities = NULL;
    ha->area_count = ha->device_count = ha->registered_count = ha->count = ha->cap = 0;
    ha->have = 0;
}

void h5_ha_free(H5Ha *ha)
{
    h5_ha_disconnect(ha);
    forget_home(ha);
}

/* ---- JSON helpers ------------------------------------------------------------------------ */

static void get_text(const H5Json *j, uint32_t object, const char *key, char *out, size_t size)
{
    uint32_t t = h5_json_get(j, object, key);
    out[0] = 0;
    if (t && (j->tokens[t].type == H5_JSON_STRING || j->tokens[t].type == H5_JSON_NUMBER)) h5_json_text(j, t, out, size);
}

static int is_null(const H5Json *j, uint32_t t)
{
    return !t || (j->tokens[t].type == H5_JSON_LITERAL && j->text[j->tokens[t].start] == 'n');
}

static double get_number(const H5Json *j, uint32_t object, const char *key, double fallback)
{
    uint32_t t = h5_json_get(j, object, key);
    return t && j->tokens[t].type == H5_JSON_NUMBER ? h5_json_number(j, t, fallback) : fallback;
}

/* ---- signing in -------------------------------------------------------------------------------- */

static int set_address(H5Ha *ha, const char *address)
{
    H5Url url;
    if (h5_url_parse(address, &url)) {
        h5_copy(ha->error, sizeof(ha->error), "That isn't an address. Type it like http://192.168.1.20:8123");
        return -1;
    }
    /* HA's port when only an address was typed. */
    if (!strstr(address, "://") && !strchr(url.host, ':') && url.port == 80 && !strchr(address, ':')) url.port = 8123;
    ha->url = url;
    h5_url_text(&url, ha->address, sizeof(ha->address));
    return 0;
}

/* A POST to HA; the answer parsed into j. The HTTP status, or -1 with no answer. */
static int post(H5Ha *ha, const char *path, const char *type, const char *body, H5Buf *answer, H5Json *j)
{
    int status = 0;
    memset(j, 0, sizeof(*j));
    if (h5_http(&ha->url, "POST", path, type, body, NULL, answer, &status, ha->error, sizeof(ha->error))) return -1;
    if (h5_json_parse(j, (const char *)answer->data, answer->len, 4096)) memset(j, 0, sizeof(*j));
    say(ha, "H5 ha POST %s status=%d", path, status);
    return status;
}

/* The tokens from /auth/token (a code, or a refresh token). 0 or -1. */
static int take_tokens(H5Ha *ha, const char *form)
{
    H5Buf answer = { 0 };
    H5Json j;
    int status = post(ha, "/auth/token", "application/x-www-form-urlencoded", form, &answer, &j), ok = -1;

    if (status == 200 && j.count) {
        char access[sizeof(ha->access)], refresh[sizeof(ha->refresh)];
        get_text(&j, 0, "access_token", access, sizeof(access));
        get_text(&j, 0, "refresh_token", refresh, sizeof(refresh));
        if (access[0]) {
            h5_copy(ha->access, sizeof(ha->access), access);
            if (refresh[0]) h5_copy(ha->refresh, sizeof(ha->refresh), refresh);
            double expires = get_number(&j, 0, "expires_in", 1800);
            ha->expires = now_s(ha) + (int64_t)expires;
            ok = 0;
        }
    } else if (status >= 400) {
        char why[64];
        get_text(&j, 0, "error", why, sizeof(why));
        if (!strcmp(why, "invalid_grant") || status == 400 || status == 403)
            h5_copy(ha->error, sizeof(ha->error), "Home Assistant no longer accepts this PS5's sign-in. Sign in again.");
        else
            h5_format(ha->error, sizeof(ha->error), "Home Assistant refused the sign-in (%d).", status);
    } else if (status >= 0) {
        h5_format(ha->error, sizeof(ha->error), "Home Assistant answered %d to the sign-in.", status);
    }
    h5_json_free(&j);
    h5_buf_free(&answer);
    return ok;
}

/* A login-flow step's answer: done (the code exchanged for tokens), the
 * two-factor code asked for, or failed with a reason. */
static H5Login flow_answer(H5Ha *ha, int status, const H5Json *j)
{
    char type[32], step[48], base[64], code[256];

    if (status < 0) return H5_LOGIN_FAILED;
    if (status == 403 || status == 429) {
        h5_copy(ha->error, sizeof(ha->error),
                "Home Assistant has blocked sign-ins from this PS5 for now, after too many tries.");
        return H5_LOGIN_FAILED;
    }
    if (!j->count) {
        h5_format(ha->error, sizeof(ha->error), "That address answered, but not like Home Assistant (%d).", status);
        return H5_LOGIN_FAILED;
    }
    get_text(j, 0, "type", type, sizeof(type));
    get_text(j, 0, "step_id", step, sizeof(step));
    uint32_t errors = h5_json_get(j, 0, "errors");
    get_text(j, errors, "base", base, sizeof(base));
    if (!strcmp(type, "create_entry")) {
        H5Buf form = { 0 };
        get_text(j, 0, "result", code, sizeof(code));
        h5_buf_puts(&form, "grant_type=authorization_code&code=");
        h5_url_encode(&form, code);
        h5_buf_puts(&form, "&client_id=");
        h5_url_encode(&form, CLIENT_ID);
        h5_buf_terminate(&form);
        int ok = form.data && code[0] ? take_tokens(ha, (const char *)form.data) : -1;
        h5_buf_free(&form);
        ha->flow[0] = 0;
        return ok ? H5_LOGIN_FAILED : H5_LOGIN_OK;
    }
    if (!strcmp(type, "form")) {
        if (!strcmp(base, "invalid_auth")) {
            h5_copy(ha->error, sizeof(ha->error), "The username or password isn't right.");
            return H5_LOGIN_FAILED;
        }
        if (!strcmp(base, "invalid_code")) {
            h5_copy(ha->error, sizeof(ha->error), "That code isn't right. Try the newest one.");
            return H5_LOGIN_CODE;
        }
        if (!strcmp(step, "mfa")) return H5_LOGIN_CODE;
        if (base[0]) {
            h5_format(ha->error, sizeof(ha->error), "Home Assistant says: %s", base);
            return H5_LOGIN_FAILED;
        }
    }
    if (!strcmp(type, "abort")) {
        char reason[64];
        get_text(j, 0, "reason", reason, sizeof(reason));
        h5_format(ha->error, sizeof(ha->error), "Home Assistant stopped the sign-in (%s).", reason);
        return H5_LOGIN_FAILED;
    }
    h5_format(ha->error, sizeof(ha->error), "Home Assistant asked for a sign-in step Home5 doesn't know (%s).",
              step[0] ? step : type);
    return H5_LOGIN_FAILED;
}

H5Login h5_ha_login(H5Ha *ha, const char *address, const char *username, const char *password)
{
    H5Buf body = { 0 }, answer = { 0 };
    H5Json j;
    H5Login result = H5_LOGIN_FAILED;
    char path[200];

    ha->error[0] = 0;
    if (set_address(ha, address)) return H5_LOGIN_FAILED;
    h5_buf_puts(&body, "{\"client_id\":");
    h5_json_quote(&body, CLIENT_ID);
    h5_buf_puts(&body, ",\"handler\":[\"homeassistant\",null],\"redirect_uri\":");
    h5_json_quote(&body, REDIRECT_URI);
    h5_buf_puts(&body, "}");
    h5_buf_terminate(&body);
    int status = body.data ? post(ha, "/auth/login_flow", "application/json", (const char *)body.data, &answer, &j) : -1;
    if (status != 200 || !j.count) {
        if (status == 404 || (status >= 0 && !j.count))
            h5_format(ha->error, sizeof(ha->error), "%s answered, but it isn't Home Assistant.", ha->address);
        else if (status >= 0)
            h5_format(ha->error, sizeof(ha->error), "Home Assistant couldn't start a sign-in (%d).", status);
        goto out;
    }
    get_text(&j, 0, "flow_id", ha->flow, sizeof(ha->flow));
    h5_json_free(&j);
    if (!ha->flow[0] || h5_format(path, sizeof(path), "/auth/login_flow/%s", ha->flow)) goto out;
    body.len = 0;
    h5_buf_puts(&body, "{\"username\":");
    h5_json_quote(&body, username);
    h5_buf_puts(&body, ",\"password\":");
    h5_json_quote(&body, password);
    h5_buf_puts(&body, ",\"client_id\":");
    h5_json_quote(&body, CLIENT_ID);
    h5_buf_puts(&body, "}");
    h5_buf_terminate(&body);
    status = post(ha, path, "application/json", (const char *)body.data, &answer, &j);
    result = flow_answer(ha, status, &j);
out:
    h5_json_free(&j);
    /* The password goes no further. */
    if (body.data) memset(body.data, 0, body.cap);
    h5_buf_free(&body);
    h5_buf_free(&answer);
    if (result == H5_LOGIN_OK) ha->phase = H5_OFFLINE;
    return result;
}

H5Login h5_ha_login_code(H5Ha *ha, const char *code)
{
    H5Buf body = { 0 }, answer = { 0 };
    H5Json j;
    char path[200];

    ha->error[0] = 0;
    if (!ha->flow[0] || h5_format(path, sizeof(path), "/auth/login_flow/%s", ha->flow)) {
        h5_copy(ha->error, sizeof(ha->error), "The sign-in has expired. Start again.");
        return H5_LOGIN_FAILED;
    }
    h5_buf_puts(&body, "{\"code\":");
    h5_json_quote(&body, code);
    h5_buf_puts(&body, ",\"client_id\":");
    h5_json_quote(&body, CLIENT_ID);
    h5_buf_puts(&body, "}");
    h5_buf_terminate(&body);
    int status = body.data ? post(ha, path, "application/json", (const char *)body.data, &answer, &j) : -1;
    H5Login result = flow_answer(ha, status, &j);
    h5_json_free(&j);
    h5_buf_free(&body);
    h5_buf_free(&answer);
    if (result == H5_LOGIN_OK) ha->phase = H5_OFFLINE;
    return result;
}

int h5_ha_use_token(H5Ha *ha, const char *address, const char *token)
{
    ha->error[0] = 0;
    if (set_address(ha, address)) return -1;
    if (strlen(token) < 20 || strlen(token) >= sizeof(ha->access)) {
        h5_copy(ha->error, sizeof(ha->error), "That doesn't look like a Home Assistant access token.");
        return -1;
    }
    h5_copy(ha->access, sizeof(ha->access), token);
    ha->refresh[0] = 0;
    ha->expires = 0;
    ha->phase = H5_OFFLINE;
    return 0;
}

int h5_ha_refresh(H5Ha *ha)
{
    if (!ha->refresh[0]) return ha->access[0] ? 0 : -1;
    if (ha->access[0] && ha->expires > now_s(ha) + 120) return 0;
    H5Buf form = { 0 };
    h5_buf_puts(&form, "grant_type=refresh_token&refresh_token=");
    h5_url_encode(&form, ha->refresh);
    h5_buf_puts(&form, "&client_id=");
    h5_url_encode(&form, CLIENT_ID);
    h5_buf_terminate(&form);
    int ok = form.data ? take_tokens(ha, (const char *)form.data) : -1;
    h5_buf_free(&form);
    return ok;
}

void h5_ha_sign_out(H5Ha *ha)
{
    h5_ha_disconnect(ha);
    if (ha->refresh[0]) {
        /* Revoked on the server, so the token is no good to anyone who finds it. */
        H5Buf form = { 0 }, answer = { 0 };
        int status = 0;
        char error[H5_ERROR];
        h5_buf_puts(&form, "token=");
        h5_url_encode(&form, ha->refresh);
        h5_buf_terminate(&form);
        if (form.data && !h5_http(&ha->url, "POST", "/auth/revoke", "application/x-www-form-urlencoded",
                                  (const char *)form.data, NULL, &answer, &status, error, sizeof(error)) &&
            status == 404) {
            /* Before HA 2022.8 */
            form.len = 0;
            h5_buf_puts(&form, "action=revoke&token=");
            h5_url_encode(&form, ha->refresh);
            h5_buf_terminate(&form);
            h5_http(&ha->url, "POST", "/auth/token", "application/x-www-form-urlencoded", (const char *)form.data, NULL,
                    &answer, &status, error, sizeof(error));
        }
        say(ha, "H5 ha signed out status=%d", status);
        h5_buf_free(&form);
        h5_buf_free(&answer);
    }
    memset(ha->access, 0, sizeof(ha->access));
    memset(ha->refresh, 0, sizeof(ha->refresh));
    ha->expires = 0;
    ha->flow[0] = 0;
    forget_home(ha);
    ha->version++;
    ha->phase = H5_SIGNED_OUT;
}

int h5_ha_save(const H5Ha *ha, const char *path)
{
    H5Buf text = { 0 };
    h5_buf_printf(&text, "# Home5's Home Assistant account (Settings > Sign out removes it)\naddress = %s\n", ha->address);
    if (ha->refresh[0]) h5_buf_printf(&text, "refresh_token = %s\n", ha->refresh);
    else h5_buf_printf(&text, "access_token = %s\n", ha->access);
    int ok = text.data ? h5_write_file(path, text.data, text.len) : -1;
    if (text.data) memset(text.data, 0, text.cap);
    h5_buf_free(&text);
    return ok;
}

int h5_ha_load(H5Ha *ha, const char *path)
{
    H5Buf text = { 0 };
    char address[sizeof(ha->address)] = "", access[sizeof(ha->access)] = "", refresh[sizeof(ha->refresh)] = "";
    int ok = -1;

    if (h5_read_file(path, &text, 64 * 1024)) goto out;
    for (char *line = (char *)text.data, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *name = h5_trim(line), *value = h5_trim(eq + 1);
        if (!strcmp(name, "address")) h5_copy(address, sizeof(address), value);
        else if (!strcmp(name, "refresh_token")) h5_copy(refresh, sizeof(refresh), value);
        else if (!strcmp(name, "access_token")) h5_copy(access, sizeof(access), value);
    }
    if (!address[0] || set_address(ha, address) || (!refresh[0] && !access[0])) goto out;
    h5_copy(ha->refresh, sizeof(ha->refresh), refresh);
    h5_copy(ha->access, sizeof(ha->access), access);
    ha->expires = 0;
    ha->phase = H5_OFFLINE;
    ok = 0;
out:
    if (text.data) memset(text.data, 0, text.cap);
    h5_buf_free(&text);
    memset(access, 0, sizeof(access));
    memset(refresh, 0, sizeof(refresh));
    return ok;
}

/* ---- the live connection ----------------------------------------------------------------------- */

static int send_text(H5Ha *ha, const char *text)
{
    if (!ha->ws.conn || h5_ws_send(&ha->ws, text, strlen(text))) return -1;
    ha->last_sent_ms = clock_ms(ha);
    return 0;
}

/* {"id":N,"type":type<more>}: N, or 0 when it couldn't be sent. */
static uint32_t ask(H5Ha *ha, const char *type, const char *more)
{
    char text[600];
    uint32_t id = ++ha->next_id;
    if (h5_format(text, sizeof(text), "{\"id\":%u,\"type\":\"%s\"%s}", (unsigned)id, type, more ? more : "") ||
        send_text(ha, text))
        return 0;
    return id;
}

/* A message's type ("auth_required"), waiting at most wait_ms. */
static int wait_message(H5Ha *ha, H5Buf *message, int wait_ms, char *type, size_t type_size)
{
    H5Json j;
    if (h5_ws_poll(&ha->ws, wait_ms, message) != 1) return -1;
    ha->last_heard_ms = clock_ms(ha);
    if (h5_json_parse(&j, (const char *)message->data, message->len, 256)) return -1;
    get_text(&j, 0, "type", type, type_size);
    h5_json_free(&j);
    return 0;
}

static int ask_everything(H5Ha *ha)
{
    ha->have = 0;
    ha->want_areas = ask(ha, "config/area_registry/list", NULL);
    ha->want_devices = ask(ha, "config/device_registry/list", NULL);
    ha->want_entities = ask(ha, "config/entity_registry/list", NULL);
    ha->want_states = ask(ha, "get_states", NULL);
    ha->want_events = ask(ha, "subscribe_events", ",\"event_type\":\"state_changed\"");
    ask(ha, "subscribe_events", ",\"event_type\":\"area_registry_updated\"");
    ask(ha, "subscribe_events", ",\"event_type\":\"device_registry_updated\"");
    ask(ha, "subscribe_events", ",\"event_type\":\"entity_registry_updated\"");
    ha->want_notes = ask(ha, "persistent_notification/subscribe", NULL);
    ha->want_config = ask(ha, "get_config", NULL);
    return ha->want_areas && ha->want_devices && ha->want_entities && ha->want_states && ha->want_events ? 0 : -1;
}

int h5_ha_connect(H5Ha *ha)
{
    H5Buf message = { 0 };
    char type[32];
    int ok = -1;

    h5_ha_disconnect(ha);
    ha->error[0] = 0;
    if (!ha->access[0] && !ha->refresh[0]) {
        ha->phase = H5_SIGNED_OUT;
        return -1;
    }
    if (h5_ha_refresh(ha)) {
        /* No new token: refused (sign in again), or just no answer (try again later). */
        if (strstr(ha->error, "no longer accepts")) ha->phase = H5_SIGNED_OUT;
        return -1;
    }
    if (h5_ws_open(&ha->ws, &ha->url, "/api/websocket", ha->error, sizeof(ha->error))) return -1;
    if (wait_message(ha, &message, AUTH_WAIT_MS, type, sizeof(type)) || strcmp(type, "auth_required")) {
        h5_format(ha->error, sizeof(ha->error), "%s didn't start Home Assistant's live connection.", ha->url.host);
        goto out;
    }
    {
        H5Buf auth = { 0 };
        h5_buf_puts(&auth, "{\"type\":\"auth\",\"access_token\":");
        h5_json_quote(&auth, ha->access);
        h5_buf_puts(&auth, "}");
        h5_buf_terminate(&auth);
        int sent = auth.data ? send_text(ha, (const char *)auth.data) : -1;
        if (auth.data) memset(auth.data, 0, auth.cap);
        h5_buf_free(&auth);
        if (sent) goto out;
    }
    if (wait_message(ha, &message, AUTH_WAIT_MS, type, sizeof(type))) {
        h5_format(ha->error, sizeof(ha->error), "%s stopped answering.", ha->url.host);
        goto out;
    }
    if (!strcmp(type, "auth_invalid")) {
        say(ha, "H5 ha auth_invalid long_lived=%d", !ha->refresh[0]);
        if (ha->refresh[0]) {
            /* Refreshed next time. */
            ha->expires = 0;
            h5_copy(ha->error, sizeof(ha->error), "Home Assistant didn't accept the sign-in; trying again.");
        } else {
            h5_copy(ha->error, sizeof(ha->error), "Home Assistant doesn't accept this access token. Sign in again.");
            ha->phase = H5_SIGNED_OUT;
            memset(ha->access, 0, sizeof(ha->access));
        }
        goto out;
    }
    if (strcmp(type, "auth_ok")) {
        h5_format(ha->error, sizeof(ha->error), "Unexpected answer from Home Assistant (%s).", type);
        goto out;
    }
    ha->next_id = 0;
    if (ask_everything(ha)) {
        h5_copy(ha->error, sizeof(ha->error), "The connection broke while asking for your home.");
        goto out;
    }
    ha->phase = H5_LOADING;
    ha->last_heard_ms = clock_ms(ha);
    say(ha, "H5 ha connected %s", ha->address);
    ok = 0;
out:
    h5_buf_free(&message);
    if (ok) {
        h5_ws_close(&ha->ws);
        if (ha->phase != H5_SIGNED_OUT) ha->phase = H5_OFFLINE;
    }
    return ok;
}

void h5_ha_disconnect(H5Ha *ha)
{
    h5_ws_close(&ha->ws);
    if (ha->phase == H5_LOADING || ha->phase == H5_READY) ha->phase = H5_OFFLINE;
}

int h5_ha_call(H5Ha *ha, const char *domain, const char *service, const char *entity, const char *data)
{
    H5Buf text = { 0 };
    uint32_t id = ++ha->next_id;
    h5_buf_printf(&text, "{\"id\":%u,\"type\":\"call_service\",\"domain\":", (unsigned)id);
    h5_json_quote(&text, domain);
    h5_buf_puts(&text, ",\"service\":");
    h5_json_quote(&text, service);
    h5_buf_puts(&text, ",\"target\":{\"entity_id\":");
    h5_json_quote(&text, entity);
    h5_buf_printf(&text, "},\"service_data\":{%s}}", data ? data : "");
    h5_buf_terminate(&text);
    int ok = text.data ? send_text(ha, (const char *)text.data) : -1;
    say(ha, "H5 ha call %s.%s %s %s id=%u sent=%d", domain, service, entity, data ? data : "", (unsigned)id, !ok);
    h5_buf_free(&text);
    return ok;
}

/* ---- the home ---------------------------------------------------------------------------------- */

static const struct { const char *domain; H5Kind kind; } domains[] = {
    { "light", H5_LIGHT },      { "switch", H5_SWITCH },  { "input_boolean", H5_SWITCH }, { "fan", H5_FAN },
    { "cover", H5_COVER },      { "climate", H5_CLIMATE }, { "lock", H5_LOCK },          { "media_player", H5_MEDIA },
    { "scene", H5_SCENE },      { "script", H5_SCRIPT },  { "button", H5_BUTTON },      { "input_button", H5_BUTTON },
    { "sensor", H5_SENSOR },    { "binary_sensor", H5_BINARY },
};

static int kind_of(const char *entity_id, H5Kind *kind)
{
    const char *dot = strchr(entity_id, '.');
    if (!dot) return -1;
    for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++)
        if (strlen(domains[i].domain) == (size_t)(dot - entity_id) && !memcmp(entity_id, domains[i].domain, (size_t)(dot - entity_id))) {
            *kind = domains[i].kind;
            return 0;
        }
    return -1;
}

const char *h5_kind_domain(H5Kind kind)
{
    for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++)
        if (domains[i].kind == kind) return domains[i].domain;
    return "homeassistant";
}

H5Entity *h5_ha_entity(H5Ha *ha, const char *id)
{
    for (int i = 0; i < ha->count; i++)
        if (!strcmp(ha->entities[i].id, id)) return &ha->entities[i];
    return NULL;
}

const H5Area *h5_ha_area(const H5Ha *ha, const char *id)
{
    for (int i = 0; i < ha->area_count; i++)
        if (!strcmp(ha->areas[i].id, id)) return &ha->areas[i];
    return NULL;
}

static int compare_registered(const void *a, const void *b)
{
    return strcmp(((const H5Registered *)a)->id, ((const H5Registered *)b)->id);
}

static const H5Registered *registered(const H5Ha *ha, const char *id)
{
    H5Registered key;
    if (!ha->registered_count) return NULL;
    h5_copy(key.id, sizeof(key.id), id);
    return bsearch(&key, ha->registered, (size_t)ha->registered_count, sizeof(H5Registered), compare_registered);
}

static const char *device_area(const H5Ha *ha, const char *device)
{
    for (int i = 0; device[0] && i < ha->device_count; i++)
        if (!strcmp(ha->devices[i].id, device)) return ha->devices[i].area;
    return "";
}

/* An entity's place, name and visibility from the registries. */
static void place(H5Ha *ha, H5Entity *e)
{
    const H5Registered *r = registered(ha, e->id);
    e->hidden = 0;
    if (!r) return;
    if (r->name[0]) h5_copy(e->name, sizeof(e->name), r->name);
    h5_copy(e->device, sizeof(e->device), r->device);
    h5_copy(e->area, sizeof(e->area), r->area[0] ? r->area : device_area(ha, r->device));
    e->hidden = r->hidden;
}

static void list_strings(const H5Json *j, uint32_t array, char out[H5_MODES][H5_MODE], uint8_t *count)
{
    *count = 0;
    if (!array || j->tokens[array].type != H5_JSON_ARRAY) return;
    uint32_t t = h5_json_first(j, array);
    for (uint32_t k = 0; k < j->tokens[array].children && *count < H5_MODES; k++, t = h5_json_next(j, t))
        if (j->tokens[t].type == H5_JSON_STRING) h5_json_text(j, t, out[(*count)++], H5_MODE);
}

static int has_string(const H5Json *j, uint32_t array, const char *text)
{
    if (!array || j->tokens[array].type != H5_JSON_ARRAY) return 0;
    uint32_t t = h5_json_first(j, array);
    for (uint32_t k = 0; k < j->tokens[array].children; k++, t = h5_json_next(j, t)) {
        char value[32];
        if (j->tokens[t].type == H5_JSON_STRING && !h5_json_text(j, t, value, sizeof(value)) && !strcmp(value, text))
            return 1;
    }
    return 0;
}

/* One state object (from get_states or a state_changed event) into the home. */
static void take_state(H5Ha *ha, const H5Json *j, uint32_t object)
{
    char id[128];
    H5Kind kind;
    H5Entity *e;

    get_text(j, object, "entity_id", id, sizeof(id));
    if (!id[0] || kind_of(id, &kind)) return;
    if (!(e = h5_ha_entity(ha, id))) {
        if (ha->count == ha->cap) {
            int cap = ha->cap ? ha->cap * 2 : 256;
            H5Entity *grown = h5_realloc(ha->entities, sizeof(H5Entity) * (size_t)cap);
            if (!grown) return;
            ha->entities = grown;
            ha->cap = cap;
        }
        e = &ha->entities[ha->count++];
        memset(e, 0, sizeof(*e));
        h5_copy(e->id, sizeof(e->id), id);
        e->kind = kind;
        place(ha, e);
    }
    uint32_t a = h5_json_get(j, object, "attributes");
    const H5Registered *r = registered(ha, id);
    get_text(j, object, "state", e->state, sizeof(e->state));
    if (!r || !r->name[0]) get_text(j, a, "friendly_name", e->name, sizeof(e->name));
    if (!e->name[0]) h5_copy(e->name, sizeof(e->name), strchr(id, '.') + 1);
    e->available = strcmp(e->state, "unavailable") && strcmp(e->state, "unknown");
    e->on = !strcmp(e->state, "on") || !strcmp(e->state, "open") || !strcmp(e->state, "playing") ||
            !strcmp(e->state, "unlocked") || !strcmp(e->state, "opening");
    e->brightness = -1;
    e->rgb = -1;
    e->position = -1;
    e->percentage = -1;
    e->current = e->target = e->min_temp = e->max_temp = NAN;
    e->step = 0.5f;
    switch (kind) {
    case H5_LIGHT: {
        uint32_t modes = h5_json_get(j, a, "supported_color_modes");
        e->color = has_string(j, modes, "hs") || has_string(j, modes, "xy") || has_string(j, modes, "rgb") ||
                   has_string(j, modes, "rgbw") || has_string(j, modes, "rgbww");
        e->temp = has_string(j, modes, "color_temp");
        int dims = e->color || e->temp || has_string(j, modes, "brightness") || has_string(j, modes, "white");
        if (dims) e->brightness = e->on ? (int16_t)get_number(j, a, "brightness", 255) : 0;
        uint32_t rgb = h5_json_get(j, a, "rgb_color");
        if (rgb && j->tokens[rgb].type == H5_JSON_ARRAY && j->tokens[rgb].children == 3) {
            int c[3];
            for (int k = 0; k < 3; k++) c[k] = (int)h5_json_number(j, h5_json_item(j, rgb, (uint32_t)k), 0);
            e->rgb = (c[0] & 255) << 16 | (c[1] & 255) << 8 | (c[2] & 255);
        }
        e->kelvin = (int16_t)get_number(j, a, "color_temp_kelvin", 0);
        e->min_kelvin = (int16_t)get_number(j, a, "min_color_temp_kelvin", 2000);
        e->max_kelvin = (int16_t)get_number(j, a, "max_color_temp_kelvin", 6500);
        break;
    }
    case H5_COVER: {
        double position = get_number(j, a, "current_position", -1);
        int features = (int)get_number(j, a, "supported_features", 0);
        if (position >= 0 && (features & 4)) e->position = (int8_t)position;
        break;
    }
    case H5_FAN:
        e->percentage = (int8_t)get_number(j, a, "percentage", -1);
        break;
    case H5_CLIMATE:
        e->current = (float)get_number(j, a, "current_temperature", NAN);
        e->target = (float)get_number(j, a, "temperature", NAN);
        e->step = (float)get_number(j, a, "target_temp_step", 0.5);
        e->min_temp = (float)get_number(j, a, "min_temp", 7);
        e->max_temp = (float)get_number(j, a, "max_temp", 35);
        list_strings(j, h5_json_get(j, a, "hvac_modes"), e->modes, &e->mode_count);
        list_strings(j, h5_json_get(j, a, "preset_modes"), e->presets, &e->preset_count);
        get_text(j, a, "preset_mode", e->preset, sizeof(e->preset));
        get_text(j, a, "hvac_action", e->action, sizeof(e->action));
        e->on = e->available && strcmp(e->state, "off");
        break;
    case H5_MEDIA:
        get_text(j, a, "media_title", e->media_title, sizeof(e->media_title));
        e->on = !strcmp(e->state, "playing");
        break;
    case H5_SENSOR:
    case H5_BINARY:
        get_text(j, a, "unit_of_measurement", e->unit, sizeof(e->unit));
        get_text(j, a, "device_class", e->device_class, sizeof(e->device_class));
        break;
    default:
        break;
    }
}

static void remove_entity(H5Ha *ha, const char *id)
{
    H5Entity *e = h5_ha_entity(ha, id);
    if (!e) return;
    int at = (int)(e - ha->entities);
    memmove(e, e + 1, sizeof(H5Entity) * (size_t)(ha->count - at - 1));
    ha->count--;
}

static void take_areas(H5Ha *ha, const H5Json *j, uint32_t list)
{
    uint32_t n = j->tokens[list].children, t = h5_json_first(j, list);
    H5Area *areas = h5_alloc(sizeof(H5Area) * (n ? n : 1));
    if (!areas) return;
    int count = 0;
    for (uint32_t k = 0; k < n; k++, t = h5_json_next(j, t)) {
        get_text(j, t, "area_id", areas[count].id, sizeof(areas[count].id));
        get_text(j, t, "name", areas[count].name, sizeof(areas[count].name));
        if (areas[count].id[0]) count++;
    }
    h5_free(ha->areas);
    ha->areas = areas;
    ha->area_count = count;
}

static void take_devices(H5Ha *ha, const H5Json *j, uint32_t list)
{
    uint32_t n = j->tokens[list].children, t = h5_json_first(j, list);
    H5Device *devices = h5_alloc(sizeof(H5Device) * (n ? n : 1));
    if (!devices) return;
    int count = 0;
    for (uint32_t k = 0; k < n; k++, t = h5_json_next(j, t)) {
        get_text(j, t, "id", devices[count].id, sizeof(devices[count].id));
        uint32_t area = h5_json_get(j, t, "area_id");
        if (!is_null(j, area)) get_text(j, t, "area_id", devices[count].area, sizeof(devices[count].area));
        if (devices[count].id[0]) count++;
    }
    h5_free(ha->devices);
    ha->devices = devices;
    ha->device_count = count;
}

static void take_entities(H5Ha *ha, const H5Json *j, uint32_t list)
{
    uint32_t n = j->tokens[list].children, t = h5_json_first(j, list);
    H5Registered *all = h5_alloc(sizeof(H5Registered) * (n ? n : 1));
    if (!all) return;
    int count = 0;
    for (uint32_t k = 0; k < n; k++, t = h5_json_next(j, t)) {
        H5Registered *r = &all[count];
        get_text(j, t, "entity_id", r->id, sizeof(r->id));
        uint32_t name = h5_json_get(j, t, "name");
        if (!is_null(j, name)) get_text(j, t, "name", r->name, sizeof(r->name));
        if (!is_null(j, h5_json_get(j, t, "area_id"))) get_text(j, t, "area_id", r->area, sizeof(r->area));
        if (!is_null(j, h5_json_get(j, t, "device_id"))) get_text(j, t, "device_id", r->device, sizeof(r->device));
        r->hidden = !is_null(j, h5_json_get(j, t, "hidden_by")) || !is_null(j, h5_json_get(j, t, "disabled_by")) ||
                    !is_null(j, h5_json_get(j, t, "entity_category"));
        if (r->id[0]) count++;
    }
    qsort(all, (size_t)count, sizeof(H5Registered), compare_registered);
    h5_free(ha->registered);
    ha->registered = all;
    ha->registered_count = count;
}

static void notifications(H5Ha *ha, const H5Json *j, uint32_t event)
{
    char type[24];
    get_text(j, event, "type", type, sizeof(type));
    if (strcmp(type, "added") || !ha->notify) return;   /* "current": the ones already there */
    uint32_t all = h5_json_get(j, event, "notifications");
    if (!all || j->tokens[all].type != H5_JSON_OBJECT) return;
    uint32_t t = h5_json_first(j, all);
    for (uint32_t k = 0; k < j->tokens[all].children / 2; k++) {
        uint32_t value = t + 1;
        char title[128], message[512];
        get_text(j, value, "title", title, sizeof(title));
        get_text(j, value, "message", message, sizeof(message));
        ha->notify(ha->context, title[0] ? title : "Home Assistant", message);
        t = h5_json_next(j, value);
    }
}

int h5_ha_feed(H5Ha *ha, const char *text, size_t length)
{
    H5Json j;
    char type[32], event_type[64];
    int changed = 0;

    /* Room for the biggest answers (get_states of a big home), never more tokens than characters. */
    uint32_t most = length / 2 + 64 < 8u * 1024 * 1024 ? (uint32_t)(length / 2 + 64) : 8u * 1024 * 1024;
    if (h5_json_parse(&j, text, length, most)) {
        say(ha, "H5 ha unreadable message (%zu bytes)", length);
        return 0;
    }
    get_text(&j, 0, "type", type, sizeof(type));
    uint32_t id = (uint32_t)get_number(&j, 0, "id", 0);
    if (!strcmp(type, "result")) {
        uint32_t result = h5_json_get(&j, 0, "result");
        if (!h5_json_true(&j, h5_json_get(&j, 0, "success"))) {
            char message[200];
            get_text(&j, h5_json_get(&j, 0, "error"), "message", message, sizeof(message));
            say(ha, "H5 ha request %u failed: %s", (unsigned)id, message);
            if (id != ha->want_notes) {
                h5_format(ha->error, sizeof(ha->error), "Home Assistant: %s", message[0] ? message : "that didn't work");
                changed = 1;
            }
        } else if (id == ha->want_config && result) {
            char unit[8];
            get_text(&j, h5_json_get(&j, result, "unit_system"), "temperature", unit, sizeof(unit));
            if (unit[0]) h5_copy(ha->temp_unit, sizeof(ha->temp_unit), unit);
            changed = 1;
        } else if (result && j.tokens[result].type == H5_JSON_ARRAY) {
            if (id == ha->want_areas) { take_areas(ha, &j, result); ha->have |= HAVE_AREAS; changed = 1; }
            else if (id == ha->want_devices) { take_devices(ha, &j, result); ha->have |= HAVE_DEVICES; changed = 1; }
            else if (id == ha->want_entities) { take_entities(ha, &j, result); ha->have |= HAVE_ENTITIES; changed = 1; }
            else if (id == ha->want_states) {
                uint32_t t = h5_json_first(&j, result);
                for (uint32_t k = 0; k < j.tokens[result].children; k++, t = h5_json_next(&j, t)) take_state(ha, &j, t);
                ha->have |= HAVE_STATES;
                changed = 1;
            }
            /* The registries changed: everyone's place again. */
            if (changed && id != ha->want_states)
                for (int i = 0; i < ha->count; i++) place(ha, &ha->entities[i]);
        }
    } else if (!strcmp(type, "event")) {
        uint32_t event = h5_json_get(&j, 0, "event");
        get_text(&j, event, "event_type", event_type, sizeof(event_type));
        if (id == ha->want_notes) {
            notifications(ha, &j, event);
        } else if (!strcmp(event_type, "state_changed")) {
            uint32_t data = h5_json_get(&j, event, "data"), state = h5_json_get(&j, data, "new_state");
            if (is_null(&j, state)) {
                char entity[128];
                get_text(&j, data, "entity_id", entity, sizeof(entity));
                remove_entity(ha, entity);
            } else {
                take_state(ha, &j, state);
            }
            changed = 1;
        } else if (!strcmp(event_type, "area_registry_updated")) {
            ha->want_areas = ask(ha, "config/area_registry/list", NULL);
        } else if (!strcmp(event_type, "device_registry_updated")) {
            ha->want_devices = ask(ha, "config/device_registry/list", NULL);
        } else if (!strcmp(event_type, "entity_registry_updated")) {
            ha->want_entities = ask(ha, "config/entity_registry/list", NULL);
        }
    }
    h5_json_free(&j);
    if (ha->phase == H5_LOADING && (ha->have & HAVE_ALL) == HAVE_ALL) {
        ha->phase = H5_READY;
        say(ha, "H5 ha ready areas=%d devices=%d entities=%d states=%d", ha->area_count, ha->device_count,
            ha->registered_count, ha->count);
        changed = 1;
    }
    if (changed) ha->version++;
    return changed;
}

int h5_ha_poll(H5Ha *ha, int timeout_ms)
{
    H5Buf message = { 0 };
    int changed = 0, got, wait = timeout_ms;

    if (!ha->ws.conn) return -1;
    /* Everything waiting, a little at a time. */
    for (int n = 0; n < 64 && (got = h5_ws_poll(&ha->ws, wait, &message)) == 1; n++) {
        ha->last_heard_ms = clock_ms(ha);
        changed |= h5_ha_feed(ha, (const char *)message.data, message.len);
        wait = 0;
    }
    h5_buf_free(&message);
    if (got < 0) {
        say(ha, "H5 ha connection lost");
        h5_copy(ha->error, sizeof(ha->error), "The connection to Home Assistant was lost. Reconnecting…");
        h5_ha_disconnect(ha);
        return -1;
    }
    uint64_t now = clock_ms(ha);
    if (now - ha->last_heard_ms > SILENT_MS) {
        say(ha, "H5 ha silent for %llu ms", (unsigned long long)(now - ha->last_heard_ms));
        h5_ha_disconnect(ha);
        return -1;
    }
    if (now - ha->last_sent_ms > PING_EVERY_MS) ask(ha, "ping", NULL);
    return changed;
}

/* ---- what a card says ------------------------------------------------------------------------- */

static void number_text(const char *state, const char *unit, char *out, size_t size)
{
    char *end;
    double value = strtod(state, &end);
    if (end != state && !*end) {
        /* At most one decimal, none when it's whole. */
        if (fabs(value - (double)(long long)value) < 0.05) snprintf(out, size, "%lld", (long long)llround(value));
        else snprintf(out, size, "%.1f", value);
    } else {
        h5_copy(out, size, state);
    }
    if (unit && unit[0]) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, " %s", unit);
    }
}

static void capitalised(const char *text, char *out, size_t size)
{
    h5_copy(out, size, text);
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

void h5_entity_status(const H5Entity *e, const char *temp_unit, char *out, size_t size)
{
    char a[64], b[64];
    out[0] = 0;
    if (!e->available && e->kind != H5_SENSOR && e->kind != H5_BINARY) {
        h5_copy(out, size, "Unavailable");
        return;
    }
    switch (e->kind) {
    case H5_LIGHT:
        if (!e->on) h5_copy(out, size, "Off");
        else if (e->brightness >= 0) snprintf(out, size, "On · %d %%", (e->brightness * 100 + 127) / 255);
        else h5_copy(out, size, "On");
        break;
    case H5_SWITCH:
        h5_copy(out, size, e->on ? "On" : "Off");
        break;
    case H5_FAN:
        if (e->on && e->percentage > 0) snprintf(out, size, "On · %d %%", e->percentage);
        else h5_copy(out, size, e->on ? "On" : "Off");
        break;
    case H5_COVER:
        if (!strcmp(e->state, "opening") || !strcmp(e->state, "closing")) capitalised(e->state, out, size);
        else if (!strcmp(e->state, "closed")) h5_copy(out, size, "Closed");
        else if (e->position >= 0 && e->position < 100) snprintf(out, size, "%d %% open", e->position);
        else h5_copy(out, size, "Open");
        break;
    case H5_CLIMATE:
        if (!h5_isnan(e->current)) snprintf(a, sizeof(a), "%.1f %s", e->current, temp_unit);
        else a[0] = 0;
        if (!e->on) {
            snprintf(out, size, "Off%s%s", a[0] ? " · " : "", a);
        } else if (!h5_isnan(e->target)) {
            capitalised(e->action[0] && strcmp(e->action, "idle") ? e->action : e->state, b, sizeof(b));
            snprintf(out, size, "%s%s%s to %.1f %s", a, a[0] ? " · " : "", b, e->target, temp_unit);
        } else {
            capitalised(e->state, b, sizeof(b));
            snprintf(out, size, "%s%s%s", a, a[0] ? " · " : "", b);
        }
        break;
    case H5_LOCK:
    case H5_SCRIPT:
        capitalised(e->state, out, size);
        if (e->kind == H5_SCRIPT && !strcmp(e->state, "off")) out[0] = 0;
        break;
    case H5_MEDIA:
        capitalised(e->state, a, sizeof(a));
        if (e->media_title[0] && e->on) snprintf(out, size, "%s · %s", a, e->media_title);
        else h5_copy(out, size, a);
        break;
    case H5_SENSOR:
        if (!e->available) h5_copy(out, size, "—");
        else number_text(e->state, e->unit, out, size);
        break;
    case H5_BINARY: {
        const char *on = "On", *off = "Off", *c = e->device_class;
        if (!strcmp(c, "door") || !strcmp(c, "window") || !strcmp(c, "opening") || !strcmp(c, "garage_door")) {
            on = "Open";
            off = "Closed";
        } else if (!strcmp(c, "motion") || !strcmp(c, "occupancy") || !strcmp(c, "presence")) {
            on = "Detected";
            off = "Clear";
        } else if (!strcmp(c, "moisture")) {
            on = "Wet";
            off = "Dry";
        } else if (!strcmp(c, "smoke") || !strcmp(c, "gas") || !strcmp(c, "problem") || !strcmp(c, "safety")) {
            on = "Alert";
            off = "OK";
        } else if (!strcmp(c, "lock")) {
            on = "Unlocked";
            off = "Locked";
        }
        h5_copy(out, size, !e->available ? "—" : !strcmp(e->state, "on") ? on : off);
        break;
    }
    default:
        break;
    }
}
