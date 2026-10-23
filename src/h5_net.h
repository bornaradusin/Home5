/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: talking to Home Assistant over the network. A connection is plain
 * TCP (http://) or TLS on mbedTLS (https://), checked against the console's
 * root certificates and Mozilla's. On it: one HTTP/1.1 request at a time
 * (sign-in, tokens), or a WebSocket (HA's live API). The sockets, names and
 * random bytes come from the platform: the console's (h5_ps5.c) or the
 * host's in the tests. */
#ifndef H5_NET_H
#define H5_NET_H
#include "h5.h"

enum { H5_NET_AGAIN = -2 };   /* nothing came (or could go) in the time given */

typedef struct H5NetOps {
    /* The IPv4 address of host, in network order; 0 or -1. */
    int (*resolve)(const char *host, uint32_t *address);
    /* A connected TCP socket, or -1, within timeout_ms. */
    int (*connect)(uint32_t address, uint16_t port, int timeout_ms);
    /* Bytes sent or received (recv: 0 when closed), -1 on an error, H5_NET_AGAIN
     * when nothing could be done in timeout_ms (0: don't wait). */
    int (*send)(int socket, const void *data, size_t size, int timeout_ms);
    int (*recv)(int socket, void *data, size_t size, int timeout_ms);
    void (*close)(int socket);
    /* Random bytes from the system; 0 or -1. */
    int (*random)(void *data, size_t size);
    /* PEM files of root certificates, all read; NULL ends the list. */
    const char *const *ca_files;
    void (*log)(const char *line);
} H5NetOps;

void h5_net_init(const H5NetOps *ops);
/* Random bytes from the platform; 0 or -1. */
int h5_net_random(void *data, size_t size);

/* ---- addresses ---------------------------------------------------------------------- */
typedef struct H5Url {
    int tls;            /* https (or wss) */
    char host[256];
    uint16_t port;
    char path[256];     /* a base path ("/ha"), or "" */
} H5Url;

/* "http://192.168.1.20:8123", "https://name.example/ha/", "192.168.1.20:8123"
 * (http then), ws:// and wss:// too. 0 or -1. */
int h5_url_parse(const char *text, H5Url *url);
/* The address as http(s)://host[:port][path], the default port left out. */
void h5_url_text(const H5Url *url, char *out, size_t size);

/* ---- connections -------------------------------------------------------------------- */
typedef struct H5Conn H5Conn;

/* Connected (and the TLS handshake done) within a few seconds, or NULL with
 * the reason in error. */
H5Conn *h5_conn_open(const H5Url *url, char *error, size_t error_size);
/* All of data, or -1. */
int h5_conn_write(H5Conn *conn, const void *data, size_t size);
/* Bytes read, 0 when the other end closed, -1 on an error, H5_NET_AGAIN when
 * nothing came in timeout_ms. */
int h5_conn_read(H5Conn *conn, void *data, size_t size, int timeout_ms);
void h5_conn_close(H5Conn *conn);

/* ---- HTTP -------------------------------------------------------------------------- */
/* One request to url's server: method, path under url's base path, an
 * optional body of content_type and an optional bearer token. The answer's
 * body in out and its status in *status. 0 when an answer came, -1 when none
 * did (the reason in error). */
int h5_http(const H5Url *url, const char *method, const char *path, const char *content_type, const char *body,
            const char *bearer, H5Buf *out, int *status, char *error, size_t error_size);

/* ---- WebSocket ------------------------------------------------------------------------ */
typedef struct H5Ws {
    H5Conn *conn;
    H5Buf in;           /* bytes read, not yet a whole frame */
    H5Buf message;      /* a message's fragments so far */
    int fragmented;     /* a text message is being put together */
} H5Ws;

/* Connects to path under url's base path and upgrades. 0 or -1 (error). */
int h5_ws_open(H5Ws *ws, const H5Url *url, const char *path, char *error, size_t error_size);
/* One text message. 0 or -1. */
int h5_ws_send(H5Ws *ws, const char *text, size_t length);
/* Waits at most timeout_ms for a whole text message: 1 when one is in
 * message (0-terminated), 0 when none came yet, -1 when the connection is
 * closed or broken. Pings are answered on the way. */
int h5_ws_poll(H5Ws *ws, int timeout_ms, H5Buf *message);
void h5_ws_close(H5Ws *ws);

/* For the tests: the masked frame a client sends. */
int h5_ws_frame(H5Buf *out, int opcode, const void *payload, size_t length, const uint8_t mask[4]);

#endif
