/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5's host checks: the network code, the Home Assistant client, the
 * layout and the screens, against tests/fake_ha.py over HTTP and HTTPS.
 *
 *   test_home5 WORK_DIR FONTS PLAIN_PORT TLS_PORT CA_PEM [SHOTS_DIR|-] [--tls]
 */
#include "h5.h"
#include "h5_draw.h"
#include "h5_ha.h"
#include "h5_json.h"
#include "h5_layout.h"
#include "h5_net.h"
#include "h5_ui.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

static const char *work, *fonts, *ca_dir, *shots;
static int plain_port, tls_port;

/* ---- the host's network --------------------------------------------------------------------------- */

static int host_resolve(const char *host, uint32_t *address)
{
    struct addrinfo hints = { 0 }, *found;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &found)) return -1;
    *address = ((struct sockaddr_in *)found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    return 0;
}

static int wait_for(int sock, short events, int timeout_ms)
{
    struct pollfd fd = { sock, events, 0 };
    int r = poll(&fd, 1, timeout_ms);
    return r > 0 ? 0 : r == 0 ? H5_NET_AGAIN : -1;
}

static int host_connect(uint32_t address, uint16_t port, int timeout_ms)
{
    struct sockaddr_in to = { 0 };
    int sock = socket(AF_INET, SOCK_STREAM, 0), error = 0;
    socklen_t length = sizeof(error);
    if (sock < 0) return -1;
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = address;
    fcntl(sock, F_SETFL, O_NONBLOCK);
    if (connect(sock, (struct sockaddr *)&to, sizeof(to)) < 0) {
        if (errno != EINPROGRESS || wait_for(sock, POLLOUT, timeout_ms) ||
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
            close(sock);
            return -1;
        }
    }
    return sock;
}

static int host_send(int sock, const void *data, size_t size, int timeout_ms)
{
    for (;;) {
        ssize_t n = send(sock, data, size, MSG_NOSIGNAL);
        if (n >= 0) return (int)n;
        if (errno != EAGAIN) return -1;
        int w = wait_for(sock, POLLOUT, timeout_ms);
        if (w) return w;
    }
}

static int host_recv(int sock, void *data, size_t size, int timeout_ms)
{
    for (;;) {
        ssize_t n = recv(sock, data, size, 0);
        if (n >= 0) return (int)n;
        if (errno != EAGAIN) return -1;
        int w = wait_for(sock, POLLIN, timeout_ms);
        if (w) return w;
    }
}

static void host_close(int sock) { close(sock); }

static int host_random(void *data, size_t size)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, data, size);
    close(fd);
    return n == (ssize_t)size ? 0 : -1;
}

static char log_text[1 << 16];
static void host_log(const char *line)
{
    if (strlen(log_text) + strlen(line) + 2 < sizeof(log_text)) {
        strcat(log_text, line);
        strcat(log_text, "\n");
    }
    if (getenv("H5_TEST_VERBOSE")) fprintf(stderr, "%s\n", line);
}

/* The UI's clock; skipped ahead to let a toast go before a screenshot for the README. */
static uint64_t clock_skip;
static uint64_t host_clock(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000 + clock_skip;
}

static int64_t host_time(void) { return (int64_t)time(NULL); }

static void url(char *out, size_t size, int tls, int port)
{
    snprintf(out, size, "%s://localhost:%d", tls ? "https" : "http", port);
}

/* ---- pieces ------------------------------------------------------------------------------------------ */

static void test_pieces(void)
{
    H5Url u;
    char text[300];
    {
        /* Shapes: a triangle is filled inside, not outside. */
        static uint32_t px[40 * 40];
        H5Canvas canvas = { px, 40, 40, 40 };
        h5_clear(&canvas, 0xff000000u);
        h5_triangle(&canvas, 20, 5, 5, 35, 35, 35, 0, 0xffffffffu);
        CHECK(px[25 * 40 + 20] == 0xffffffffu && px[6 * 40 + 6] == 0xff000000u, "a triangle: %08x inside, %08x outside",
              px[25 * 40 + 20], px[6 * 40 + 6]);
    }
    CHECK(!h5_url_parse("http://192.168.1.20:8123", &u) && !u.tls && !strcmp(u.host, "192.168.1.20") && u.port == 8123 &&
              !u.path[0], "plain address");
    CHECK(!h5_url_parse(" https://home.example/ha/ ", &u) && u.tls && u.port == 443 && !strcmp(u.path, "/ha"), "https, base path");
    CHECK(!h5_url_parse("homeassistant.local:8123", &u) && !u.tls && u.port == 8123, "no scheme");
    CHECK(h5_url_parse("ftp://x", &u) && h5_url_parse("http://:80", &u) && h5_url_parse("http://a:99999", &u) &&
              h5_url_parse("http://user@host", &u), "bad addresses refused");
    h5_url_parse("https://home.example:8443/ha", &u);
    h5_url_text(&u, text, sizeof(text));
    CHECK(!strcmp(text, "https://home.example:8443/ha"), "address text: %s", text);

    /* RFC 6455's own example */
    uint8_t digest[20];
    char accept[40];
    const char *both = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    h5_sha1(both, strlen(both), digest);
    h5_base64(digest, 20, accept, sizeof(accept));
    CHECK(!strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "websocket accept: %s", accept);
    h5_sha1("", 0, digest);
    h5_base64(digest, 20, accept, sizeof(accept));
    CHECK(!strcmp(accept, "2jmj7l5rSw0yVb/vlWAYkK/YBwk="), "sha1 of nothing: %s", accept);
    h5_base64("ab", 2, accept, sizeof(accept));
    CHECK(!strcmp(accept, "YWI="), "base64 padding");

    H5Buf b = { 0 };
    h5_json_quote(&b, "say \"hi\"\n\x01 é");
    h5_buf_terminate(&b);
    CHECK(!strcmp((char *)b.data, "\"say \\\"hi\\\"\\n\\u0001 é\""), "json quote: %s", b.data);
    b.len = 0;
    h5_url_encode(&b, "a b&c=d/é");
    h5_buf_terminate(&b);
    CHECK(!strcmp((char *)b.data, "a%20b%26c%3Dd%2F%C3%A9"), "url encode: %s", b.data);
    b.len = 0;
    uint8_t mask[4] = { 1, 2, 3, 4 }, big[70000];
    memset(big, 'x', sizeof(big));
    h5_ws_frame(&b, 1, "hi", 2, mask);
    CHECK(b.len == 8 && b.data[0] == 0x81 && b.data[1] == 0x82 && b.data[6] == ('h' ^ 1) && b.data[7] == ('i' ^ 2), "small frame");
    b.len = 0;
    h5_ws_frame(&b, 1, big, sizeof(big), mask);
    CHECK(b.len == 14 + sizeof(big) && b.data[1] == (0x80 | 127) && b.data[7] == 0x01 && b.data[8] == 0x11 && b.data[9] == 0x70,
          "64-bit length frame");
    h5_buf_free(&b);
    char copied[6];
    h5_copy(copied, sizeof(copied), "abcdé");
    CHECK(!strcmp(copied, "abcd"), "copy keeps characters whole: %s", copied);

    H5Entity e = { 0 };
    char status[100];
    e.kind = H5_LIGHT;
    e.available = 1;
    e.on = 1;
    e.brightness = 204;
    h5_entity_status(&e, "°C", status, sizeof(status));
    CHECK(!strcmp(status, "On · 80 %"), "light status: %s", status);
    e.kind = H5_SENSOR;
    strcpy(e.state, "21.43");
    strcpy(e.unit, "°C");
    h5_entity_status(&e, "°C", status, sizeof(status));
    CHECK(!strcmp(status, "21.4 °C"), "sensor status: %s", status);
    e.kind = H5_BINARY;
    strcpy(e.device_class, "door");
    strcpy(e.state, "off");
    h5_entity_status(&e, "°C", status, sizeof(status));
    CHECK(!strcmp(status, "Closed"), "door status: %s", status);
}

/* ---- the client ------------------------------------------------------------------------------------- */

static char told[512];
static int told_count;
static void notified(void *context, const char *title, const char *message)
{
    (void)context;
    told_count++;
    snprintf(told, sizeof(told), "%s: %s", title, message);
}

static void client(H5Ha *ha)
{
    h5_ha_init(ha);
    ha->clock_ms = host_clock;
    ha->now = host_time;
    ha->log = host_log;
    ha->notify = notified;
}

/* Polls until the home is loaded (or a few seconds pass). */
static int until_ready(H5Ha *ha)
{
    uint64_t end = host_clock() + 8000;
    while (ha->phase != H5_READY && host_clock() < end)
        if (h5_ha_poll(ha, 100) < 0) return -1;
    return ha->phase == H5_READY ? 0 : -1;
}

/* Polls until entity's state is value. */
static int until_state(H5Ha *ha, const char *id, const char *value)
{
    uint64_t end = host_clock() + 5000;
    for (;;) {
        H5Entity *e = h5_ha_entity(ha, id);
        if (e && !strcmp(e->state, value)) return 0;
        if (host_clock() > end || h5_ha_poll(ha, 100) < 0) return -1;
    }
}

static void test_client(int tls)
{
    H5Ha ha;
    char address[100], account[600];
    url(address, sizeof(address), tls, tls ? tls_port : plain_port);
    snprintf(account, sizeof(account), "%s/account-%d.txt", work, tls);

    client(&ha);
    CHECK(h5_ha_login(&ha, address, "kevin", "wrong") == H5_LOGIN_FAILED && strstr(ha.error, "password isn't right"),
          "wrong password: %s", ha.error);
    CHECK(h5_ha_login(&ha, address, "kevin", "secret") == H5_LOGIN_OK && ha.access[0] && !strncmp(ha.refresh, "RT-", 3) &&
              ha.phase == H5_OFFLINE, "signed in (tls %d): %s", tls, ha.error);
    CHECK(!h5_ha_save(&ha, account), "account saved");
    h5_ha_free(&ha);

    client(&ha);
    CHECK(!h5_ha_load(&ha, account) && ha.refresh[0] && !ha.access[0] && ha.phase == H5_OFFLINE, "account read back");
    CHECK(!h5_ha_connect(&ha) && ha.phase == H5_LOADING && ha.access[0], "connected with a refreshed token: %s", ha.error);
    CHECK(!until_ready(&ha), "home loaded: %s", ha.error);
    H5Entity *ceiling = h5_ha_entity(&ha, "light.ceiling"), *bedside = h5_ha_entity(&ha, "light.bedroom");
    H5Entity *climate = h5_ha_entity(&ha, "climate.living"), *blinds = h5_ha_entity(&ha, "cover.blinds");
    CHECK(ceiling && !strcmp(ceiling->area, "living_room") && ceiling->on && ceiling->brightness == 204 && ceiling->temp &&
              !ceiling->color && ceiling->kelvin == 2700, "a light by its device's area");
    CHECK(bedside && !strcmp(bedside->name, "Bedside") && bedside->color && bedside->rgb == 0xff9a62, "a colour light");
    CHECK(climate && climate->target == 22 && fabsf(climate->current - 21.4f) < 0.01f && climate->mode_count == 4 &&
              climate->preset_count == 3 && !strcmp(climate->preset, "home") && !strcmp(climate->action, "heating"),
          "the thermostat");
    CHECK(blinds && blinds->position == 60, "the blinds");
    CHECK(h5_ha_entity(&ha, "light.hidden")->hidden && h5_ha_entity(&ha, "sensor.living_rssi")->hidden &&
              !h5_ha_entity(&ha, "sun.sun") && h5_ha_entity(&ha, "sensor.filler_119"),
          "hidden, diagnostic and unknown kinds; big messages");
    CHECK(ha.area_count == 4 && !strcmp(ha.temp_unit, "°C"), "areas and units");

    /* A service call, its state change coming back. */
    CHECK(!h5_ha_call(&ha, "light", "turn_off", "light.ceiling", NULL) && !until_state(&ha, "light.ceiling", "off") &&
              !h5_ha_entity(&ha, "light.ceiling")->on, "turned off");
    CHECK(!h5_ha_call(&ha, "climate", "set_temperature", "climate.living", "\"temperature\":22.5"), "a temperature");
    uint64_t end = host_clock() + 3000;
    while (h5_ha_entity(&ha, "climate.living")->target != 22.5f && host_clock() < end) h5_ha_poll(&ha, 50);
    CHECK(h5_ha_entity(&ha, "climate.living")->target == 22.5f, "the new target");
    /* A failed call: said why. */
    h5_ha_call(&ha, "light", "explode", "light.ceiling", NULL);
    end = host_clock() + 3000;
    while (!strstr(ha.error, "not found") && host_clock() < end) h5_ha_poll(&ha, 50);
    CHECK(strstr(ha.error, "explode not found") != NULL, "failure told: %s", ha.error);
    /* A notification as it comes, not the ones already there. */
    told_count = 0;
    h5_ha_call(&ha, "script", "turn_on", "script.notify_test", NULL);
    end = host_clock() + 3000;
    while (!told_count && host_clock() < end) h5_ha_poll(&ha, 50);
    CHECK(told_count == 1 && !strcmp(told, "Doorbell: Someone is at the door"), "notified: %d %s", told_count, told);
    /* An area renamed in HA: asked for again. */
    h5_ha_call(&ha, "script", "turn_on", "script.rename_kitchen", NULL);
    end = host_clock() + 3000;
    while (strcmp(h5_ha_area(&ha, "kitchen")->name, "Cookhouse") && host_clock() < end) h5_ha_poll(&ha, 50);
    CHECK(!strcmp(h5_ha_area(&ha, "kitchen")->name, "Cookhouse"), "areas refreshed");
    /* The connection dropped by the server. */
    h5_ha_call(&ha, "script", "turn_on", "script.drop", NULL);
    int got = 0;
    end = host_clock() + 3000;
    while ((got = h5_ha_poll(&ha, 50)) >= 0 && host_clock() < end) {}
    CHECK(got < 0 && ha.phase == H5_OFFLINE, "a dropped connection noticed");
    CHECK(!h5_ha_connect(&ha) && !until_ready(&ha), "and connected again");

    h5_ha_sign_out(&ha);
    CHECK(ha.phase == H5_SIGNED_OUT && !ha.refresh[0] && !ha.count, "signed out");
    h5_ha_free(&ha);

    /* Two-factor: the code asked for, a wrong one, the right one. */
    client(&ha);
    CHECK(h5_ha_login(&ha, address, "mfa", "secret") == H5_LOGIN_CODE, "a code asked for");
    CHECK(h5_ha_login_code(&ha, "000000") == H5_LOGIN_CODE && strstr(ha.error, "code isn't right"), "wrong code");
    CHECK(h5_ha_login_code(&ha, "123456") == H5_LOGIN_OK && ha.refresh[0], "right code");
    h5_ha_free(&ha);

    /* A long-lived token. */
    client(&ha);
    CHECK(!h5_ha_use_token(&ha, address, "LLT-0123456789abcdefghijklmnopqrstuvwxyz") && !h5_ha_connect(&ha) &&
              !until_ready(&ha), "a long-lived token");
    h5_ha_free(&ha);
    client(&ha);
    CHECK(!h5_ha_use_token(&ha, address, "LLT-not-the-right-one-at-all-xxxxxxxxxx") && h5_ha_connect(&ha) &&
              ha.phase == H5_SIGNED_OUT && strstr(ha.error, "doesn't accept this access token"), "a wrong token: %s", ha.error);
    h5_ha_free(&ha);
}

static void test_refusals(void)
{
    H5Ha ha;
    char address[100];
    client(&ha);
    /* An HTTPS server whose certificate the PS5 has no root for (the TLS test CA isn't loaded yet). */
    url(address, sizeof(address), 1, tls_port);
    CHECK(h5_ha_login(&ha, address, "kevin", "secret") == H5_LOGIN_FAILED && strstr(ha.error, "isn't one the PS5 can trust"),
          "untrusted certificate: %s", ha.error);
    CHECK(h5_ha_login(&ha, "http://localhost:1", "kevin", "secret") == H5_LOGIN_FAILED && strstr(ha.error, "No answer"),
          "nothing there: %s", ha.error);
    CHECK(h5_ha_login(&ha, "http://no-such-host.invalid:8123", "a", "b") == H5_LOGIN_FAILED && strstr(ha.error, "can't find"),
          "no such host: %s", ha.error);
    CHECK(h5_ha_login(&ha, "not an address at all", "a", "b") == H5_LOGIN_FAILED && strstr(ha.error, "isn't an address"),
          "not an address: %s", ha.error);
    h5_ha_free(&ha);
}

/* ---- the layout --------------------------------------------------------------------------------------- */

static int room_index(const H5View *v, const char *id)
{
    for (int i = 0; i < v->room_count; i++)
        if (!strcmp(v->rooms[i].id, id)) return i;
    return -1;
}

static int room_has(const H5Ha *ha, const H5View *v, const char *room, const char *entity)
{
    int r = room_index(v, room);
    if (r < 0) return 0;
    for (int i = 0; i < v->rooms[r].count; i++)
        if (!strcmp(ha->entities[v->items[v->rooms[r].first + i]].id, entity)) return 1;
    return 0;
}

static void test_layout(void)
{
    H5Ha ha;
    H5Layout *layout = calloc(1, sizeof(H5Layout)), *back = calloc(1, sizeof(H5Layout));
    H5View view;
    char address[100], path[600], living[64], bedroom[64];
    url(address, sizeof(address), 0, plain_port);
    snprintf(path, sizeof(path), "%s/layout.txt", work);

    client(&ha);
    h5_ha_use_token(&ha, address, "LLT-0123456789abcdefghijklmnopqrstuvwxyz");
    CHECK(!h5_ha_connect(&ha) && !until_ready(&ha), "connected for the layout");
    h5_layout_reset(layout);
    h5_layout_view(layout, &ha, &view);
    CHECK(view.room_count == 0 && ha.area_count == 4, "no rooms to start with, whatever HA's areas are");
    h5_view_free(&view);

    /* A room made, its devices, a scene and a sensor put in; one HA doesn't have is kept, unshown. */
    CHECK(!h5_layout_new_room(layout, "Living room", living, sizeof(living)) && !strcmp(living, "ps5-1"), "a room");
    CHECK(h5_layout_new_room(layout, "", bedroom, sizeof(bedroom)), "a room needs a name");
    h5_layout_add(layout, living, "sensor.living_humidity");
    h5_layout_add(layout, living, "scene.movie");
    h5_layout_add(layout, living, "light.lamp");
    h5_layout_add(layout, living, "light.ceiling");
    h5_layout_add(layout, living, "light.gone");
    h5_layout_add(layout, living, "light.ceiling");
    CHECK(layout->item_count == 5 && h5_layout_add(layout, "ps5-99", "light.lamp"), "once each, and only into a room");
    h5_layout_view(layout, &ha, &view);
    CHECK(view.room_count == 1 && view.rooms[0].count == 4 && room_has(&ha, &view, living, "light.ceiling") &&
              !room_has(&ha, &view, living, "light.gone"), "what HA has: %d", view.room_count ? view.rooms[0].count : -1);
    const char *order[] = { "light.ceiling", "light.lamp", "scene.movie", "sensor.living_humidity" };
    int sorted = view.room_count == 1;
    for (int i = 0; sorted && i < 4; i++) sorted = !strcmp(ha.entities[view.items[view.rooms[0].first + i]].id, order[i]);
    CHECK(sorted, "lights first (Ceiling light, Floor lamp), then the scene, then the sensor");
    h5_view_free(&view);

    /* A second room; the kettle in both; renamed; moved left; kept in the file. */
    h5_layout_new_room(layout, "Bedroom", bedroom, sizeof(bedroom));
    h5_layout_add(layout, bedroom, "light.bedroom");
    h5_layout_add(layout, bedroom, "switch.kettle");
    h5_layout_add(layout, living, "switch.kettle");
    h5_layout_rename(layout, "light.ceiling", "Big light");
    h5_layout_rename(layout, living, "Lounge");
    h5_layout_rename(layout, living, "");
    h5_layout_move_room(layout, bedroom, -1);
    h5_layout_move_room(layout, bedroom, -1);
    h5_layout_view(layout, &ha, &view);
    CHECK(view.room_count == 2 && !strcmp(view.rooms[0].id, bedroom) && !strcmp(view.rooms[1].name, "Lounge") &&
              room_has(&ha, &view, bedroom, "switch.kettle") && room_has(&ha, &view, living, "switch.kettle"),
          "moved left, renamed (never to nothing), the kettle in two rooms");
    CHECK(!strcmp(h5_layout_name(layout, "light.ceiling", "x"), "Big light"), "a device renamed");
    h5_view_free(&view);
    CHECK(!h5_layout_save(layout, path) && !h5_layout_load(back, path) && back->room_count == 2 &&
              !strcmp(back->rooms[0].id, bedroom) && back->item_count == 8 && back->name_count == 1 && back->next_room == 3,
          "kept in the file: %d rooms, %d items", back->room_count, back->item_count);
    h5_layout_remove(layout, living, "switch.kettle");
    CHECK(!h5_layout_has(layout, living, "switch.kettle") && h5_layout_has(layout, bedroom, "switch.kettle"), "taken out of one");
    h5_layout_delete_room(layout, bedroom);
    CHECK(layout->room_count == 1 && !h5_layout_has(layout, bedroom, "light.bedroom") && layout->item_count == 5,
          "a room deleted, with what was in it");
    h5_layout_rename(layout, "light.ceiling", "");
    CHECK(!strcmp(h5_layout_name(layout, "light.ceiling", "HA"), "HA"), "HA's name back");

    /* 1.0.0's file: its rooms made on the PS5, in its order, with what was added to them; HA's areas go. */
    const char *old =
        "# Home5's rooms on this PS5\n"
        "room = ps5-3 | Gaming corner\nroom = ps5-1 | Den\n"
        "order = living_room\norder = ps5-1\norder = -\norder = ps5-3\n"
        "add = ps5-3 | light.lamp\nadd = bedroom | switch.kettle\nremove = kitchen | sensor.x\n"
        "name = light.ceiling | Big light\nname = living_room | Lounge\nname = | Elsewhere\n";
    h5_write_file(path, old, strlen(old));
    CHECK(!h5_layout_load(back, path) && back->room_count == 2 && !strcmp(back->rooms[0].name, "Den") &&
              !strcmp(back->rooms[1].name, "Gaming corner") && back->item_count == 1 &&
              !strcmp(back->items[0].entity, "light.lamp") && back->name_count == 1 && back->next_room == 4,
          "1.0.0's layout: %d rooms (%s first), %d items, %d names", back->room_count,
          back->room_count ? back->rooms[0].name : "", back->item_count, back->name_count);
    h5_ha_free(&ha);
    free(layout);
    free(back);
}

/* ---- the screens ------------------------------------------------------------------------------------------- */

static char asked[200];
static int asked_kind = -1, keyboards;
static int fake_keyboard(void *context, const char *title, const char *text, int kind)
{
    (void)context;
    (void)text;
    keyboards++;
    snprintf(asked, sizeof(asked), "%s", title);
    asked_kind = kind;
    return 0;
}

static H5Canvas canvas;

static void frame(H5Ui *ui, uint32_t keys)
{
    H5Input input = { keys };
    h5_ui_update(ui, &input);
    h5_ui_draw(ui, &canvas);
}

static int until_screen(H5Ui *ui, const char *name)
{
    uint64_t end = host_clock() + 8000;
    while (strcmp(h5_ui_screen(ui), name) && host_clock() < end) {
        frame(ui, 0);
        usleep(10000);
    }
    return strcmp(h5_ui_screen(ui), name) ? -1 : 0;
}

/* Down the picker until entity is focused. */
static int pick_to(H5Ui *ui, const char *entity)
{
    for (int i = 0; i < 300 && strcmp(h5_ui_picked(ui), entity); i++) frame(ui, H5_KEY_DOWN);
    return !strcmp(h5_ui_picked(ui), entity);
}

/* The menu opened (Options) and down to the line that starts with text. */
static int menu_to(H5Ui *ui, const char *text)
{
    frame(ui, H5_KEY_OPTIONS);
    for (int i = 0; i < 30 && strncmp(h5_ui_menu_line(ui), text, strlen(text)); i++) frame(ui, H5_KEY_DOWN);
    return !strncmp(h5_ui_menu_line(ui), text, strlen(text));
}

/* Past the toast's few seconds, and drawn again. */
static void quiet(H5Ui *ui)
{
    clock_skip += 3500;
    frame(ui, 0);
}

static void shot(const char *name)
{
    char file[700];
    if (!shots) return;
    snprintf(file, sizeof(file), "%s/%s.ppm", shots, name);
    FILE *f = fopen(file, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", canvas.w, canvas.h);
    for (int i = 0; i < canvas.w * canvas.h; i++) {
        uint32_t p = canvas.px[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int pixel_near(int x, int y, uint32_t want)
{
    uint32_t p = canvas.px[y * canvas.w + x];
    int dr = (int)(p >> 16 & 255) - (int)(want >> 16 & 255), dg = (int)(p >> 8 & 255) - (int)(want >> 8 & 255),
        db = (int)(p & 255) - (int)(want & 255);
    return abs(dr) < 12 && abs(dg) < 12 && abs(db) < 12;
}

static void test_screens(void)
{
    char data[600], address[100];
    static const char *const usb[] = { NULL, NULL };
    snprintf(data, sizeof(data), "%s/ui", work);
    url(address, sizeof(address), 0, plain_port);
    H5Platform platform = { NULL, host_clock, host_time, 3600, fake_keyboard, notified, host_log, 0, data, usb };
    canvas.w = 1920;
    canvas.h = 1080;
    canvas.stride = 1920;
    canvas.px = calloc(1920 * 1080, 4);

    H5Ui *ui = h5_ui_create(&platform, fonts);
    CHECK(ui && !strcmp(h5_ui_screen(ui), "sign-in"), "starts at sign-in");
    frame(ui, 0);
    shot("01-sign-in");
    /* Sign in without filling in: told; then the keyboard for each field. */
    frame(ui, H5_KEY_OPTIONS);
    CHECK(!strcmp(h5_ui_screen(ui), "sign-in") && keyboards == 0, "nothing typed: stays");
    frame(ui, H5_KEY_UP);
    frame(ui, H5_KEY_UP);
    frame(ui, H5_KEY_UP);
    frame(ui, H5_KEY_CROSS);
    CHECK(keyboards == 1 && strstr(asked, "address") && asked_kind == H5_TEXT_ADDRESS, "the keyboard for the address: %s",
          asked);
    h5_ui_text(ui, 1, address);
    frame(ui, H5_KEY_DOWN);
    frame(ui, H5_KEY_CROSS);
    CHECK(asked_kind == H5_TEXT_USERNAME, "the username typed as it is (no capitals)");
    h5_ui_text(ui, 1, "kevin");
    frame(ui, H5_KEY_DOWN);
    frame(ui, H5_KEY_CROSS);
    CHECK(asked_kind == H5_TEXT_PASSWORD, "the password typed hidden");
    h5_ui_text(ui, 1, "wrong");
    frame(ui, H5_KEY_DOWN);
    frame(ui, H5_KEY_CROSS);
    CHECK(!until_screen(ui, "sign-in"), "a refused sign-in comes back");
    frame(ui, 0);
    shot("02-sign-in-refused");
    frame(ui, H5_KEY_UP);
    frame(ui, H5_KEY_CROSS);
    h5_ui_text(ui, 1, "secret");
    frame(ui, H5_KEY_OPTIONS);
    CHECK(!strcmp(h5_ui_screen(ui), "busy"), "signing in");
    shot("03-signing-in");
    CHECK(!until_screen(ui, "home"), "signed in and home: %s", h5_ui_screen(ui));
    uint64_t end = host_clock() + 8000;
    while (h5_ui_ha(ui)->phase != H5_READY && host_clock() < end) frame(ui, 0);
    frame(ui, 0);
    CHECK(h5_ui_ha(ui)->phase == H5_READY && h5_ui_view(ui)->room_count == 0, "no rooms of HA's own: %d",
          h5_ui_view(ui)->room_count);
    char account[700];
    snprintf(account, sizeof(account), "%s/account.txt", data);
    CHECK(h5_is_file(account), "the account kept");
    shot("04-first-room");

    /* No button on the screen for it: ✕ only says where. Options: New room is the menu's first line; the room's name,
     * then straight to what goes in it. */
    keyboards = 0;
    frame(ui, H5_KEY_CROSS);
    CHECK(keyboards == 0 && !strcmp(h5_ui_screen(ui), "home"), "✕ with no rooms opens nothing");
    frame(ui, H5_KEY_OPTIONS);
    CHECK(!strcmp(h5_ui_menu_line(ui), "New room"), "the menu starts with New room: %s", h5_ui_menu_line(ui));
    frame(ui, H5_KEY_CROSS);
    CHECK(keyboards == 1 && strstr(asked, "room's name") && asked_kind == H5_TEXT_NAME, "the room's keyboard: %s", asked);
    h5_ui_text(ui, 1, "Living room");
    frame(ui, 0);
    CHECK(!strcmp(h5_ui_screen(ui), "pick") && h5_ui_layout(ui)->room_count == 1, "made, and the picker: %s",
          h5_ui_screen(ui));
    /* △: a search, any case, spaces at the ends left out; every word somewhere in the name, area or id. */
    int everything = h5_ui_found(ui);
    keyboards = 0;
    frame(ui, H5_KEY_TRIANGLE);
    CHECK(keyboards == 1 && strstr(asked, "Search") && asked_kind == H5_TEXT_SEARCH, "△: the search's keyboard: %s", asked);
    h5_ui_text(ui, 1, "  LAMP ");
    frame(ui, 0);
    CHECK(h5_ui_found(ui) == 1 && !strcmp(h5_ui_picked(ui), "light.lamp"), "“lamp”: the floor lamp alone (%d)",
          h5_ui_found(ui));
    frame(ui, H5_KEY_TRIANGLE);
    h5_ui_text(ui, 1, "living temp");
    frame(ui, 0);
    CHECK(h5_ui_found(ui) == 1 && !strcmp(h5_ui_picked(ui), "sensor.living_temperature"), "“living temp”: %s",
          h5_ui_picked(ui));
    frame(ui, H5_KEY_TRIANGLE);
    /* (the kitchen is the Cookhouse in HA since the client's test) */
    h5_ui_text(ui, 1, "kettle cookhouse");
    frame(ui, 0);
    CHECK(h5_ui_found(ui) == 1 && !strcmp(h5_ui_picked(ui), "switch.kettle"), "“kettle cookhouse”: by its area in HA (%d, %s)",
          h5_ui_found(ui), h5_ui_picked(ui));
    frame(ui, H5_KEY_TRIANGLE);
    h5_ui_text(ui, 1, "light.bed");
    frame(ui, 0);
    CHECK(h5_ui_found(ui) == 1 && !strcmp(h5_ui_picked(ui), "light.bedroom"), "by entity id: %s", h5_ui_picked(ui));
    frame(ui, H5_KEY_TRIANGLE);
    h5_ui_text(ui, 1, "nothing like this");
    frame(ui, 0);
    CHECK(h5_ui_found(ui) == 0 && !h5_ui_picked(ui)[0], "nothing found");
    frame(ui, H5_KEY_CROSS);
    CHECK(h5_ui_layout(ui)->item_count == 0, "✕ with nothing found adds nothing");
    quiet(ui);
    shot("05a-pick-nothing");
    /* A cancelled keyboard keeps the search; □ clears it. */
    frame(ui, H5_KEY_TRIANGLE);
    h5_ui_text(ui, 0, "");
    CHECK(h5_ui_found(ui) == 0 && !strcmp(h5_ui_screen(ui), "pick"), "cancelled: still searching");
    frame(ui, H5_KEY_SQUARE);
    CHECK(h5_ui_found(ui) == everything, "□: everything again (%d of %d)", h5_ui_found(ui), everything);
    /* Searching among lights for a switch: all kinds, rather than nothing. */
    frame(ui, H5_KEY_R2);
    int lights = h5_ui_found(ui);
    frame(ui, H5_KEY_TRIANGLE);
    h5_ui_text(ui, 1, "kettle");
    frame(ui, 0);
    CHECK(lights == 3 && !strcmp(h5_ui_picked(ui), "switch.kettle"), "“kettle” with the lights shown: %d, %s", lights,
          h5_ui_picked(ui));
    frame(ui, H5_KEY_TRIANGLE);
    h5_ui_text(ui, 1, "living");
    frame(ui, 0);
    quiet(ui);
    shot("05b-pick-search");
    frame(ui, H5_KEY_SQUARE);
    CHECK(h5_ui_found(ui) == everything, "cleared again");
    /* R2: lights only (Bedside; Floor lamp, Living room Ceiling light); the two in the living room in. */
    frame(ui, H5_KEY_R2);
    CHECK(pick_to(ui, "light.ceiling"), "the lights: the ceiling light");
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_UP);
    CHECK(!strcmp(h5_ui_picked(ui), "light.lamp"), "then the floor lamp: %s", h5_ui_picked(ui));
    frame(ui, H5_KEY_CROSS);
    /* Switches, then blinds, heating: in; locks; no media (skipped); scenes; sensors. */
    frame(ui, H5_KEY_R2);
    CHECK(!strcmp(h5_ui_picked(ui), "switch.kettle"), "the switches: %s", h5_ui_picked(ui));
    frame(ui, H5_KEY_R2);
    CHECK(!strcmp(h5_ui_picked(ui), "cover.blinds"), "the blinds: %s", h5_ui_picked(ui));
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_R2);
    CHECK(!strcmp(h5_ui_picked(ui), "climate.living"), "the heating: %s", h5_ui_picked(ui));
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_R2);
    CHECK(!strcmp(h5_ui_picked(ui), "lock.front_door"), "the locks: %s", h5_ui_picked(ui));
    frame(ui, H5_KEY_R2);
    CHECK(pick_to(ui, "scene.movie"), "the scenes (no media between)");
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_R2);
    CHECK(pick_to(ui, "sensor.living_temperature"), "the sensors");
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_UP);
    frame(ui, H5_KEY_CROSS);
    quiet(ui);
    shot("05-pick");
    H5Layout *l = h5_ui_layout(ui);
    CHECK(l->item_count == 7 && h5_layout_has(l, l->rooms[0].id, "sensor.living_humidity") &&
              h5_layout_has(l, l->rooms[0].id, "scene.movie") && !h5_layout_has(l, l->rooms[0].id, "switch.kettle"),
          "seven things in the living room: %d", l->item_count);
    /* Taken out again with ✕, and back in. */
    frame(ui, H5_KEY_CROSS);
    CHECK(l->item_count == 6, "out again");
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_CIRCLE);
    CHECK(!strcmp(h5_ui_screen(ui), "home") && h5_ui_view(ui)->room_count == 1 && h5_ui_view(ui)->rooms[0].count == 7,
          "home with the room");

    /* The cards, four across: Ceiling light, Floor lamp, Blinds, Thermostat; Movie night (no card that adds: that's
     * in the menu). Back from the picker, the first card is focused. */
    /* The ceiling light is off (since the client's test): the light bar is the console's; on with ✕, the light
     * bar in its colour; off again. */
    H5Entity *ceiling = h5_ha_entity(h5_ui_ha(ui), "light.ceiling");
    CHECK(ceiling && !ceiling->on && h5_ui_light_bar(ui) == -1 && h5_ui_triggers(ui),
          "an off light: the console's light bar (%d, %x, %d)", ceiling ? ceiling->on : -1, (unsigned)h5_ui_light_bar(ui),
          h5_ui_triggers(ui));
    frame(ui, H5_KEY_CROSS);
    end = host_clock() + 3000;
    while (strcmp(h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->state, "on") && host_clock() < end) frame(ui, 0);
    CHECK(!strcmp(h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->state, "on") && h5_ui_light_bar(ui) == 0xffd9a0,
          "on in HA, the light bar warm white: %x", (unsigned)h5_ui_light_bar(ui));
    quiet(ui);
    shot("06-home-light-on");
    frame(ui, H5_KEY_CROSS);
    end = host_clock() + 3000;
    while (strcmp(h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->state, "off") && host_clock() < end) frame(ui, 0);
    CHECK(!strcmp(h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->state, "off") && h5_ui_light_bar(ui) == -1, "and off");
    /* R2 three times: on at 30 %. */
    frame(ui, H5_KEY_R2);
    frame(ui, H5_KEY_R2);
    frame(ui, H5_KEY_R2);
    end = host_clock() + 3000;
    while (h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->brightness != 77 && host_clock() < end) frame(ui, 0);
    CHECK(h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->brightness == 77, "brightened to 30 %%: %d",
          h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->brightness);
    /* △: its own screen; neutral white with →. */
    frame(ui, H5_KEY_TRIANGLE);
    CHECK(!strcmp(h5_ui_screen(ui), "light"), "the light's screen");
    frame(ui, H5_KEY_RIGHT);
    end = host_clock() + 3000;
    while (h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->kelvin != 4000 && host_clock() < end) frame(ui, 0);
    CHECK(h5_ha_entity(h5_ui_ha(ui), "light.ceiling")->kelvin == 4000, "neutral white");
    quiet(ui);
    shot("07-light");
    frame(ui, H5_KEY_CIRCLE);
    CHECK(!strcmp(h5_ui_screen(ui), "home"), "back from the light");
    /* The blinds: L2 closes them a step. */
    frame(ui, H5_KEY_RIGHT);
    frame(ui, H5_KEY_RIGHT);
    int position = h5_ha_entity(h5_ui_ha(ui), "cover.blinds")->position;
    frame(ui, H5_KEY_L2);
    end = host_clock() + 3000;
    while (h5_ha_entity(h5_ui_ha(ui), "cover.blinds")->position != position - 10 && host_clock() < end) frame(ui, 0);
    CHECK(h5_ha_entity(h5_ui_ha(ui), "cover.blinds")->position == position - 10, "blinds down a step");
    /* The thermostat (next to them): its screen, half a degree up, cooling, heating again. */
    frame(ui, H5_KEY_RIGHT);
    frame(ui, H5_KEY_CROSS);
    CHECK(!strcmp(h5_ui_screen(ui), "climate"), "the thermostat's screen: %s", h5_ui_screen(ui));
    float target = h5_ha_entity(h5_ui_ha(ui), "climate.living")->target;
    frame(ui, H5_KEY_R2);
    end = host_clock() + 3000;
    while (h5_ha_entity(h5_ui_ha(ui), "climate.living")->target != target + 0.5f && host_clock() < end) frame(ui, 0);
    CHECK(h5_ha_entity(h5_ui_ha(ui), "climate.living")->target == target + 0.5f, "half a degree up");
    frame(ui, H5_KEY_RIGHT);
    frame(ui, H5_KEY_CROSS);
    end = host_clock() + 3000;
    while (strcmp(h5_ha_entity(h5_ui_ha(ui), "climate.living")->state, "cool") && host_clock() < end) frame(ui, 0);
    CHECK(!strcmp(h5_ha_entity(h5_ui_ha(ui), "climate.living")->state, "cool"), "cool mode");
    frame(ui, H5_KEY_LEFT);
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_DOWN);
    quiet(ui);
    shot("08-thermostat");
    frame(ui, H5_KEY_CIRCLE);
    CHECK(!strcmp(h5_ui_screen(ui), "home"), "back home");
    /* The scene, under the first row (down from the thermostat goes to the last card there is): run with ✕. */
    frame(ui, H5_KEY_DOWN);
    frame(ui, H5_KEY_CROSS);
    end = host_clock() + 3000;
    while (strcmp(h5_ha_entity(h5_ui_ha(ui), "scene.movie")->state, "2026-10-23T09:00:00+00:00") && host_clock() < end)
        frame(ui, 0);
    CHECK(!strcmp(h5_ha_entity(h5_ui_ha(ui), "scene.movie")->state, "2026-10-23T09:00:00+00:00"), "the scene run");
    frame(ui, 0);
    shot("09-home-scene");

    /* No "New room" tab after the rooms: up to the tabs, right stays on the living room. */
    frame(ui, H5_KEY_UP);
    frame(ui, H5_KEY_UP);
    frame(ui, H5_KEY_RIGHT);
    keyboards = 0;
    frame(ui, H5_KEY_CROSS);
    CHECK(keyboards == 0 && !strcmp(h5_ui_screen(ui), "home"), "nothing past the last room: %d", keyboards);
    /* A second room from the menu. */
    CHECK(menu_to(ui, "New room"), "Options › New room");
    frame(ui, H5_KEY_CROSS);
    CHECK(keyboards == 1 && strstr(asked, "room's name"), "the room's keyboard from the menu: %d", keyboards);
    h5_ui_text(ui, 1, "Bedroom");
    frame(ui, 0);
    CHECK(!strcmp(h5_ui_screen(ui), "pick"), "its picker");
    frame(ui, H5_KEY_R2);
    CHECK(!strcmp(h5_ui_picked(ui), "light.bedroom"), "the bedside light first: %s", h5_ui_picked(ui));
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_R2);
    frame(ui, H5_KEY_CROSS);
    frame(ui, H5_KEY_CIRCLE);
    CHECK(l->room_count == 2 && h5_layout_has(l, l->rooms[1].id, "switch.kettle") &&
              h5_layout_has(l, l->rooms[1].id, "light.bedroom") && !strcmp(h5_ui_view(ui)->rooms[1].name, "Bedroom"),
          "the bedroom with the bedside light and the kettle");
    CHECK(h5_ha_entity(h5_ui_ha(ui), "switch.kettle") && !strcmp(h5_ha_entity(h5_ui_ha(ui), "switch.kettle")->area, "kitchen"),
          "the kettle still in its own area in HA");
    frame(ui, 0);
    shot("10-bedroom");
    /* Its menu, with the bedside light focused: Add or take out, Remove the light, Rename it, Rename the room, Move left,
     * New room, Delete, …; moved left. */
    frame(ui, H5_KEY_OPTIONS);
    CHECK(!strcmp(h5_ui_screen(ui), "menu"), "the menu");
    frame(ui, 0);
    shot("11-menu");
    for (int i = 0; i < 4; i++) frame(ui, H5_KEY_DOWN);
    CHECK(!strcmp(h5_ui_menu_line(ui), "Move Bedroom left"), "fifth: %s", h5_ui_menu_line(ui));
    frame(ui, H5_KEY_CROSS);
    CHECK(!strcmp(l->rooms[0].name, "Bedroom") && !strcmp(h5_ui_view(ui)->rooms[0].name, "Bedroom"), "moved left: %s",
          l->rooms[0].name);
    /* L1/R1 between rooms: the living room again, its floor lamp taken out from the menu. */
    frame(ui, H5_KEY_R1);
    frame(ui, H5_KEY_RIGHT);
    frame(ui, H5_KEY_OPTIONS);
    frame(ui, H5_KEY_DOWN);
    frame(ui, H5_KEY_CROSS);
    CHECK(!h5_layout_has(l, l->rooms[1].id, "light.lamp") && h5_layout_has(l, l->rooms[1].id, "light.ceiling"),
          "the floor lamp out of the living room");
    /* A third room from the menu, then deleted from its tab with □: asked first, ○ keeps it, ✕ deletes it. */
    CHECK(menu_to(ui, "New room"), "Options › New room again");
    frame(ui, H5_KEY_CROSS);
    h5_ui_text(ui, 1, "Spare room");
    frame(ui, H5_KEY_CIRCLE);
    CHECK(l->room_count == 3 && !strcmp(l->rooms[2].name, "Spare room"), "a third room: %d", l->room_count);
    /* Empty: no card to add with; ✕ and ↓ only say where adding is (the menu's first line). */
    frame(ui, H5_KEY_DOWN);
    frame(ui, H5_KEY_CROSS);
    CHECK(!strcmp(h5_ui_screen(ui), "home"), "✕ in an empty room opens nothing: %s", h5_ui_screen(ui));
    frame(ui, H5_KEY_OPTIONS);
    frame(ui, H5_KEY_CROSS);
    CHECK(!strcmp(h5_ui_screen(ui), "pick"), "Options › Add to the room: the picker (%s)", h5_ui_screen(ui));
    frame(ui, H5_KEY_CIRCLE);
    CHECK(!strcmp(h5_ui_screen(ui), "home") && h5_ui_view(ui)->rooms[2].count == 0, "back, still empty");
    frame(ui, H5_KEY_UP);
    quiet(ui);
    shot("12-tab-focused");
    frame(ui, H5_KEY_SQUARE);
    CHECK(!strcmp(h5_ui_screen(ui), "confirm"), "□ on its tab: asked first (%s)", h5_ui_screen(ui));
    frame(ui, 0);
    shot("12b-delete-room");
    frame(ui, H5_KEY_CIRCLE);
    CHECK(!strcmp(h5_ui_screen(ui), "home") && l->room_count == 3, "○ keeps it");
    frame(ui, H5_KEY_SQUARE);
    frame(ui, H5_KEY_CROSS);
    CHECK(!strcmp(h5_ui_screen(ui), "home") && l->room_count == 2 && h5_ui_view(ui)->room_count == 2 &&
              !strcmp(l->rooms[1].name, "Living room"), "✕ deletes it: %d rooms", l->room_count);
    h5_ui_destroy(ui);

    /* Started again: connects by itself with the kept account, and the rooms are still there. */
    ui = h5_ui_create(&platform, fonts);
    CHECK(ui && !strcmp(h5_ui_screen(ui), "busy"), "connects at start");
    CHECK(!until_screen(ui, "home"), "home again");
    end = host_clock() + 8000;
    while (h5_ui_ha(ui)->phase != H5_READY && host_clock() < end) frame(ui, 0);
    CHECK(h5_ui_view(ui)->room_count == 2 && !strcmp(h5_ui_view(ui)->rooms[0].name, "Bedroom"), "the two rooms: %d",
          h5_ui_view(ui)->room_count);
    frame(ui, 0);
    CHECK(pixel_near(10, 10, 0xff0b0f17u), "the background");
    shot("13-restarted");
    /* Signed out from the menu, asked first. */
    frame(ui, H5_KEY_OPTIONS);
    for (int i = 0; i < 20; i++) frame(ui, H5_KEY_DOWN);
    frame(ui, H5_KEY_CROSS);
    CHECK(!strcmp(h5_ui_screen(ui), "confirm"), "sign out asked");
    frame(ui, 0);
    shot("14-sign-out");
    frame(ui, H5_KEY_CROSS);
    CHECK(!until_screen(ui, "sign-in") && !h5_is_file(account), "signed out, the account gone");
    h5_ui_destroy(ui);
    free(canvas.px);
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: test_home5 WORK FONTS PLAIN_PORT TLS_PORT CA_PATH [SHOTS]\n");
        return 2;
    }
    work = argv[1];
    fonts = argv[2];
    plain_port = atoi(argv[3]);
    tls_port = atoi(argv[4]);
    ca_dir = argv[5];
    shots = argc > 6 && strcmp(argv[6], "-") ? argv[6] : NULL;

    static const char *roots_none[] = { NULL };
    H5NetOps ops = { host_resolve, host_connect, host_send, host_recv, host_close, host_random, roots_none, host_log,
                     NULL };
    h5_net_init(&ops);
    if (argc > 7 && !strcmp(argv[7], "--tls")) {
        /* HTTPS with the test CA in a .crt file: roots are read once. */
        const char *ca_dirs[] = { ca_dir, NULL };
        ops.ca_dirs = ca_dirs;
        h5_net_init(&ops);
        test_client(1);
        printf("test_home5 (https): %d checks, %d failed\n", checks, failures);
    } else {
        test_pieces();
        test_refusals();
        test_client(0);
        test_layout();
        test_screens();
        printf("test_home5: %d checks, %d failed\n", checks, failures);
    }
    if (failures && getenv("H5_TEST_LOG")) fputs(log_text, stderr);
    return failures ? 1 : 0;
}
