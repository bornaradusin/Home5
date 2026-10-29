/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Home5: connections, HTTP and WebSocket (h5_net.h), TLS on mbedTLS 3.6
 * (Apache-2.0) configured by h5_mbedtls_config.h. The TLS set-up follows
 * PC5's pc5_net.c. */
#include "h5_net.h"

/* mbedTLS as tools/build.sh builds it (the same settings, or its structures differ). */
#ifndef MBEDTLS_USER_CONFIG_FILE
#define MBEDTLS_USER_CONFIG_FILE "h5_mbedtls_config.h"
#endif
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum {
    CONNECT_MS = 6000,
    IO_MS = 15000,
    HEADER_MAX = 64 * 1024,
    ANSWER_MAX = 16 * 1024 * 1024,
    MESSAGE_MAX = 64 * 1024 * 1024,
    READ_CHUNK = 64 * 1024,
};

static H5NetOps ops;
static int have_ops, crypto_state;     /* crypto_state: 0 not tried, 1 ready, -1 failed */
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt roots;
static int root_count;

#define USER_AGENT "Home5/" H5_VERSION " (PS5 homebrew; +" H5_REPO ")"

static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *format, ...)
{
    char line[600];
    va_list args;
    if (!have_ops || !ops.log) return;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ops.log(line);
}

void h5_net_init(const H5NetOps *platform)
{
    ops = *platform;
    have_ops = 1;
}

int h5_net_random(void *data, size_t size)
{
    return have_ops && ops.random ? ops.random(data, size) : -1;
}

/* ---- what mbedTLS asks of the platform (h5_mbedtls_config.h) ---------------------------- */

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length, size_t *written);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length, size_t *written)
{
    (void)data;
    *written = 0;
    if (h5_net_random(output, length)) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *written = length;
    return 0;
}

/* UTC calendar time, for certificate dates (Howard Hinnant's civil_from_days). */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm)
{
    long long t = (long long)*tt, days = t / 86400, rest = t % 86400;
    if (rest < 0) { rest += 86400; days--; }
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = (int)(y + (m <= 2) - 1900);
    tm->tm_mon = (int)m - 1;
    tm->tm_mday = (int)d;
    tm->tm_hour = (int)(rest / 3600);
    tm->tm_min = (int)(rest % 3600 / 60);
    tm->tm_sec = (int)(rest % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);   /* 1970-01-01 was a Thursday */
    tm->tm_yday = (int)(doy >= 306 ? doy - 306 : doy + 59 + ((y % 4 == 0 && y % 100) || y % 400 == 0));
    return tm;
}

static void *tls_calloc(size_t count, size_t size)
{
    return count && size > SIZE_MAX / count ? NULL : h5_alloc(count * size);
}

static void load_ca_file(const char *file)
{
    H5Buf pem = { 0 };
    if (!h5_read_file(file, &pem, 4 * 1024 * 1024) && !h5_buf_terminate(&pem)) {
        /* PEM wants its closing 0 counted. */
        int status = mbedtls_x509_crt_parse(&roots, pem.data, pem.len + 1);
        int count = 0;
        for (const mbedtls_x509_crt *c = &roots; c && c->version; c = c->next) count++;
        note("H5 net roots file=%s certificates=%d refused=%d", file, count - root_count, status > 0 ? status : 0);
        root_count = count;
    } else {
        note("H5 net roots file=%s unreadable", file);
    }
    h5_buf_free(&pem);
}

static void load_ca_entry(const char *path, const char *name)
{
    size_t length = strlen(name);
    char file[H5_PATH];
    if (length < 4 || strcmp(name + length - 4, ".crt")) return;
    if (h5_format(file, sizeof(file), "%s/%s", path, name)) {
        note("H5 net roots path too long in directory=%s", path);
        return;
    }
    for (const char *const *known = ops.ca_files; known && *known; known++) {
        const char *known_name = strrchr(*known, '/');
        known_name = known_name ? known_name + 1 : *known;
        if (!strcmp(name, known_name)) return;
    }
    load_ca_file(file);
}

static void load_ca_directory(const char *path)
{
#if !defined(__FreeBSD__)
    /* Host tests use the portable directory stream API. */
    DIR *dir = opendir(path);
    if (!dir) {
        note("H5 net roots directory=%s unreadable", path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) load_ca_entry(path, entry->d_name);
    closedir(dir);
#else
    /* The PS5 target is FreeBSD-based; read its directory-entry records directly. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        note("H5 net roots directory=%s unreadable", path);
        return;
    }
    struct stat info;
    if (fstat(fd, &info) || !S_ISDIR(info.st_mode)) {
        note("H5 net roots directory=%s unreadable", path);
        close(fd);
        return;
    }
    size_t entries_size = info.st_blksize > 0 ? (size_t)info.st_blksize : 8192;
    if (entries_size < 8192) entries_size = 8192;
    if (entries_size > 1024 * 1024) {
        note("H5 net roots directory=%s block size too large", path);
        close(fd);
        return;
    }
    char *entries = h5_alloc(entries_size);
    if (!entries) {
        note("H5 net roots directory=%s out of memory", path);
        close(fd);
        return;
    }
    long base = 0;
    ssize_t got;
    while ((got = getdirentries(fd, entries, (int)entries_size, &base)) > 0) {
        for (size_t offset = 0; offset < (size_t)got;) {
            const struct dirent *entry = (const struct dirent *)(entries + offset);
            size_t record = entry->d_reclen;
            if (record < offsetof(struct dirent, d_name) + 1 || record > (size_t)got - offset) break;
            load_ca_entry(path, entry->d_name);
            offset += record;
        }
    }
    if (got < 0) note("H5 net roots directory=%s read failed", path);
    h5_free(entries);
    close(fd);
#endif
}

/* The random generator and the root certificates, once. */
static int crypto_ready(void)
{
    if (crypto_state) return crypto_state > 0;
    crypto_state = -1;
    mbedtls_platform_set_calloc_free(tls_calloc, h5_free);
    int status = (int)psa_crypto_init();
    if (status) { note("H5 net psa_crypto_init=%d", status); return 0; }
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    if ((status = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"home5", 5))) {
        note("H5 net random seed=-%#x", (unsigned)-status);
        return 0;
    }
    mbedtls_x509_crt_init(&roots);
    for (const char *const *dir = ops.ca_dirs; dir && *dir; dir++) load_ca_directory(*dir);
    for (const char *const *file = ops.ca_files; file && *file; file++) load_ca_file(*file);
    crypto_state = 1;
    return 1;
}

/* ---- addresses ------------------------------------------------------------------------- */

int h5_url_parse(const char *text, H5Url *url)
{
    char work[600];
    const char *p, *slash, *colon;
    size_t host_length;

    memset(url, 0, sizeof(*url));
    h5_copy(work, sizeof(work), text ? text : "");
    p = h5_trim(work);
    if (!strncmp(p, "https://", 8)) { url->tls = 1; p += 8; }
    else if (!strncmp(p, "wss://", 6)) { url->tls = 1; p += 6; }
    else if (!strncmp(p, "http://", 7)) p += 7;
    else if (!strncmp(p, "ws://", 5)) p += 5;
    else if (strstr(p, "://")) return -1;
    slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);
    colon = memchr(p, ':', (size_t)(slash - p));
    host_length = (size_t)((colon ? colon : slash) - p);
    if (!host_length || host_length >= sizeof(url->host)) return -1;
    for (size_t i = 0; i < host_length; i++)
        if (p[i] == ' ' || p[i] == '@' || p[i] == '?' || p[i] == '#') return -1;
    memcpy(url->host, p, host_length);
    url->port = url->tls ? 443 : 80;
    if (colon) {
        char *end;
        long value = strtol(colon + 1, &end, 10);
        if (value <= 0 || value > 65535 || end != slash) return -1;
        url->port = (uint16_t)value;
    }
    /* A base path, without its last slash. */
    h5_copy(url->path, sizeof(url->path), slash);
    size_t n = strlen(url->path);
    while (n && url->path[n - 1] == '/') url->path[--n] = 0;
    if (strpbrk(url->path, " ?#")) return -1;
    return 0;
}

void h5_url_text(const H5Url *url, char *out, size_t size)
{
    char port[16] = "";
    if (url->port != (url->tls ? 443 : 80)) snprintf(port, sizeof(port), ":%u", (unsigned)url->port);
    snprintf(out, size, "%s://%s%s%s", url->tls ? "https" : "http", url->host, port, url->path);
}

/* ---- connections ------------------------------------------------------------------------- */

struct H5Conn {
    int sock, tls;
    int wait_ms;                /* what the next socket read may wait */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
};

static int bio_send(void *context, const unsigned char *data, size_t size)
{
    H5Conn *conn = context;
    int got = ops.send(conn->sock, data, size, IO_MS);
    if (got == H5_NET_AGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return got > 0 ? got : -0x004E;   /* MBEDTLS_ERR_NET_SEND_FAILED */
}

static int bio_recv(void *context, unsigned char *data, size_t size)
{
    H5Conn *conn = context;
    int got = ops.recv(conn->sock, data, size, conn->wait_ms);
    if (got == H5_NET_AGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
    return got >= 0 ? got : -0x004C;  /* MBEDTLS_ERR_NET_RECV_FAILED */
}

/* "192.168.1.20" in network order, without asking the resolver. */
static int ipv4_literal(const char *host, uint32_t *address)
{
    unsigned part[4];
    char tail;
    if (sscanf(host, "%u.%u.%u.%u%c", &part[0], &part[1], &part[2], &part[3], &tail) != 4) return -1;
    for (int i = 0; i < 4; i++)
        if (part[i] > 255) return -1;
    uint8_t bytes[4] = { (uint8_t)part[0], (uint8_t)part[1], (uint8_t)part[2], (uint8_t)part[3] };
    memcpy(address, bytes, 4);
    return 0;
}

H5Conn *h5_conn_open(const H5Url *url, char *error, size_t error_size)
{
    H5Conn *conn;
    uint32_t address = 0;
    int r;

    if (!have_ops) {
        h5_copy(error, error_size, "The network isn't ready.");
        return NULL;
    }
    if (ipv4_literal(url->host, &address) && ops.resolve(url->host, &address)) {
        note("H5 net no address for %s", url->host);
        if (strlen(url->host) > 6 && !h5_casecmp(url->host + strlen(url->host) - 6, ".local"))
            h5_format(error, error_size, "The PS5 can't find %s. Use its IP address instead.", url->host);
        else
            h5_format(error, error_size, "The PS5 can't find %s.", url->host);
        return NULL;
    }
    if (!(conn = h5_alloc(sizeof(*conn)))) {
        h5_copy(error, error_size, "Out of memory.");
        return NULL;
    }
    conn->wait_ms = IO_MS;
    if ((conn->sock = ops.connect(address, url->port, CONNECT_MS)) < 0) {
        note("H5 net can't connect to %s:%u", url->host, (unsigned)url->port);
        h5_format(error, error_size, "No answer from %s on port %u.", url->host, (unsigned)url->port);
        h5_free(conn);
        return NULL;
    }
    if (!url->tls) return conn;

    conn->tls = 1;
    mbedtls_ssl_init(&conn->ssl);
    mbedtls_ssl_config_init(&conn->conf);
    if (!crypto_ready()) {
        h5_copy(error, error_size, "HTTPS isn't available.");
        goto fail;
    }
    if ((r = mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)))
        goto tls_fail;
    mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conn->conf, &roots, NULL);
    mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random, &drbg);
    if ((r = mbedtls_ssl_setup(&conn->ssl, &conn->conf)) || (r = mbedtls_ssl_set_hostname(&conn->ssl, url->host)))
        goto tls_fail;
    mbedtls_ssl_set_bio(&conn->ssl, conn, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&conn->ssl))) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* The socket waited IO_MS for nothing. */
            h5_format(error, error_size, "%s stopped answering.", url->host);
            goto fail;
        }
        goto tls_fail;
    }
    return conn;

tls_fail: {
        char why[120];
        uint32_t flags = mbedtls_ssl_get_verify_result(&conn->ssl);
        mbedtls_strerror(r, why, sizeof(why));
        note("H5 net TLS with %s failed: -%#x %s (verify %#x)", url->host, (unsigned)-r, why, (unsigned)flags);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
            h5_format(error, error_size,
                      "%s's certificate isn't one the PS5 can trust (self-signed, or for another name). "
                      "Use its http:// address, or a certificate for this name.", url->host);
        else
            h5_format(error, error_size, "A secure connection to %s failed.", url->host);
    }
fail:
    h5_conn_close(conn);
    return NULL;
}

int h5_conn_write(H5Conn *conn, const void *data, size_t size)
{
    const uint8_t *p = data;
    size_t done = 0;
    while (done < size) {
        int r;
        if (conn->tls) {
            conn->wait_ms = IO_MS;
            r = mbedtls_ssl_write(&conn->ssl, p + done, size - done);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
        } else {
            r = ops.send(conn->sock, p + done, size - done, IO_MS);
        }
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

int h5_conn_read(H5Conn *conn, void *data, size_t size, int timeout_ms)
{
    if (!conn->tls) return ops.recv(conn->sock, data, size, timeout_ms);
    conn->wait_ms = timeout_ms;
    for (;;) {
        int r = mbedtls_ssl_read(&conn->ssl, data, size);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return H5_NET_AGAIN;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r < 0 ? -1 : r;
    }
}

void h5_conn_close(H5Conn *conn)
{
    if (!conn) return;
    if (conn->tls) {
        conn->wait_ms = 0;
        mbedtls_ssl_close_notify(&conn->ssl);
        mbedtls_ssl_free(&conn->ssl);
        mbedtls_ssl_config_free(&conn->conf);
    }
    if (conn->sock >= 0) ops.close(conn->sock);
    h5_free(conn);
}

/* ---- HTTP -------------------------------------------------------------------------------- */

/* Where needle first is in data, or NULL. */
static const char *find(const void *data, size_t size, const char *needle)
{
    size_t n = strlen(needle);
    const char *p = data;
    for (size_t i = 0; size >= n && i <= size - n; i++)
        if (!memcmp(p + i, needle, n)) return p + i;
    return NULL;
}

static int header_value(const char *headers, const char *name, char *out, size_t size)
{
    size_t length = strlen(name);
    for (const char *line = strstr(headers, "\r\n"); line && line[2]; line = strstr(line + 2, "\r\n")) {
        const char *p = line + 2;
        size_t k = 0;
        while (k < length && p[k] && h5_lower((unsigned char)p[k]) == h5_lower((unsigned char)name[k])) k++;
        if (k == length && p[k] == ':') {
            p += k + 1;
            while (*p == ' ' || *p == '\t') p++;
            size_t n = 0;
            while (p[n] && p[n] != '\r' && n + 1 < size) { out[n] = p[n]; n++; }
            out[n] = 0;
            return 0;
        }
    }
    return -1;
}

/* The body of a raw answer, in place: chunks joined, or cut to its length. */
static int take_body(H5Buf *answer, size_t header_length, const char *headers)
{
    char value[64];
    uint8_t *body = answer->data + header_length;
    size_t length = answer->len - header_length;

    if (!header_value(headers, "Transfer-Encoding", value, sizeof(value)) && strstr(value, "chunked")) {
        size_t in = 0, out = 0;
        for (;;) {
            char *end;
            if (in >= length) return -1;
            unsigned long size = strtoul((char *)body + in, &end, 16);
            const char *eol = strstr((char *)body + in, "\r\n");
            if (!eol || end == (char *)body + in) return -1;
            in = (size_t)((const uint8_t *)eol - body) + 2;
            if (!size) break;
            if (size > length - in) return -1;
            memmove(answer->data + out, body + in, size);
            out += size;
            in += size + 2;
        }
        answer->len = out;
    } else {
        if (!header_value(headers, "Content-Length", value, sizeof(value))) {
            size_t declared = (size_t)strtoull(value, NULL, 10);
            if (declared > length) return -1;   /* cut short */
            length = declared;
        }
        memmove(answer->data, body, length);
        answer->len = length;
    }
    return h5_buf_terminate(answer);
}

/* Host header: the port only when it isn't the scheme's. */
static void host_header(const H5Url *url, char *out, size_t size)
{
    if (url->port == (url->tls ? 443 : 80)) h5_copy(out, size, url->host);
    else snprintf(out, size, "%s:%u", url->host, (unsigned)url->port);
}

/* Whether a raw answer so far holds all of its body (by Content-Length). */
static int answer_complete(const H5Buf *answer)
{
    char value[64];
    const char *end = find(answer->data, answer->len, "\r\n\r\n");
    if (!end) return 0;
    size_t header_length = (size_t)(end - (const char *)answer->data) + 4;
    char *headers = h5_alloc(header_length + 1);
    if (!headers) return 0;
    memcpy(headers, answer->data, header_length);
    int done = 0;
    if (!header_value(headers, "Content-Length", value, sizeof(value)))
        done = answer->len - header_length >= (size_t)strtoull(value, NULL, 10);
    h5_free(headers);
    return done;
}

int h5_http(const H5Url *url, const char *method, const char *path, const char *content_type, const char *body,
            const char *bearer, H5Buf *out, int *status, char *error, size_t error_size)
{
    H5Buf request = { 0 };
    char host[300];
    H5Conn *conn;
    int ok = -1;

    out->len = 0;
    *status = 0;
    if (!(conn = h5_conn_open(url, error, error_size))) return -1;
    host_header(url, host, sizeof(host));
    h5_buf_printf(&request, "%s %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT
                  "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
                  method, url->path, path, host);
    if (bearer && *bearer) h5_buf_printf(&request, "Authorization: Bearer %s\r\n", bearer);
    if (body) h5_buf_printf(&request, "Content-Type: %s\r\nContent-Length: %zu\r\n", content_type, strlen(body));
    h5_buf_puts(&request, "\r\n");
    if (body) h5_buf_puts(&request, body);
    if (!request.data || h5_conn_write(conn, request.data, request.len)) {
        h5_format(error, error_size, "Couldn't send to %s.", url->host);
        goto out;
    }
    for (;;) {
        if (h5_buf_reserve(out, READ_CHUNK)) goto out;
        int r = h5_conn_read(conn, out->data + out->len, READ_CHUNK, IO_MS);
        if (r == 0) break;
        if (r < 0) {
            /* Some servers close without notifying: what came is what there is. */
            if (out->len && r != H5_NET_AGAIN) break;
            h5_format(error, error_size, "%s stopped answering.", url->host);
            goto out;
        }
        out->len += (size_t)r;
        if (out->len > ANSWER_MAX) {
            h5_format(error, error_size, "%s's answer was too big.", url->host);
            goto out;
        }
        if (answer_complete(out)) break;
    }
    if (h5_buf_terminate(out)) goto out;
    char *end = strstr((char *)out->data, "\r\n\r\n");
    if (!end || strncmp((char *)out->data, "HTTP/1.", 7) || out->len < 12) {
        h5_format(error, error_size, "%s didn't answer like Home Assistant.", url->host);
        goto out;
    }
    *end = 0;
    *status = atoi((char *)out->data + 9);
    if (take_body(out, (size_t)(end - (char *)out->data) + 4, (char *)out->data)) {
        h5_format(error, error_size, "%s's answer was cut short.", url->host);
        goto out;
    }
    ok = 0;
out:
    h5_buf_free(&request);
    h5_conn_close(conn);
    return ok;
}

/* ---- WebSocket ------------------------------------------------------------------------------ */

int h5_ws_frame(H5Buf *out, int opcode, const void *payload, size_t length, const uint8_t mask[4])
{
    uint8_t head[14];
    size_t n = 0;
    head[n++] = (uint8_t)(0x80 | (opcode & 15));
    if (length < 126) {
        head[n++] = (uint8_t)(0x80 | length);
    } else if (length < 65536) {
        head[n++] = 0x80 | 126;
        head[n++] = (uint8_t)(length >> 8);
        head[n++] = (uint8_t)length;
    } else {
        head[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[n++] = (uint8_t)((uint64_t)length >> (8 * i));
    }
    memcpy(head + n, mask, 4);
    n += 4;
    if (h5_buf_put(out, head, n) || h5_buf_reserve(out, length)) return -1;
    const uint8_t *p = payload;
    for (size_t i = 0; i < length; i++) out->data[out->len + i] = p[i] ^ mask[i & 3];
    out->len += length;
    return 0;
}

static int send_frame(H5Ws *ws, int opcode, const void *payload, size_t length)
{
    H5Buf frame = { 0 };
    uint8_t mask[4];
    if (h5_net_random(mask, 4)) return -1;
    int r = h5_ws_frame(&frame, opcode, payload, length, mask) || h5_conn_write(ws->conn, frame.data, frame.len) ? -1 : 0;
    h5_buf_free(&frame);
    return r;
}

int h5_ws_open(H5Ws *ws, const H5Url *url, const char *path, char *error, size_t error_size)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t nonce[16], digest[20];
    char key[32], expect[40], accept[80], host[300], line[96];
    H5Buf request = { 0 };

    memset(ws, 0, sizeof(*ws));
    if (h5_net_random(nonce, sizeof(nonce)) || h5_base64(nonce, sizeof(nonce), key, sizeof(key))) {
        h5_copy(error, error_size, "No random numbers for the connection.");
        return -1;
    }
    {
        char both[80];
        snprintf(both, sizeof(both), "%s%s", key, guid);
        h5_sha1(both, strlen(both), digest);
        h5_base64(digest, sizeof(digest), expect, sizeof(expect));
    }
    if (!(ws->conn = h5_conn_open(url, error, error_size))) return -1;
    host_header(url, host, sizeof(host));
    h5_buf_printf(&request,
                  "GET %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                  url->path, path, host, key);
    int r = !request.data || h5_conn_write(ws->conn, request.data, request.len);
    h5_buf_free(&request);
    if (r) {
        h5_format(error, error_size, "Couldn't send to %s.", url->host);
        goto fail;
    }
    /* The answer's headers; anything after them is already frames. */
    for (;;) {
        const char *end = ws->in.len ? find(ws->in.data, ws->in.len, "\r\n\r\n") : NULL;
        if (end) {
            size_t header_length = (size_t)(end - (const char *)ws->in.data) + 4;
            char *headers = h5_alloc(header_length + 1);
            if (!headers) goto fail;
            memcpy(headers, ws->in.data, header_length - 2);
            int status = strncmp(headers, "HTTP/1.", 7) ? 0 : atoi(headers + 9);
            int good = status == 101 && !header_value(headers, "Sec-WebSocket-Accept", accept, sizeof(accept)) &&
                       !strcmp(accept, expect);
            sscanf(headers, "%95[^\r]", line);
            h5_free(headers);
            if (!good) {
                note("H5 net WebSocket refused: %s", line);
                if (status == 404) h5_format(error, error_size, "%s has no Home Assistant API at that address.", url->host);
                else h5_format(error, error_size, "%s refused the live connection (%s).", url->host, line);
                goto fail;
            }
            h5_buf_consume(&ws->in, header_length);
            return 0;
        }
        if (ws->in.len > HEADER_MAX || h5_buf_reserve(&ws->in, 4096)) goto fail;
        r = h5_conn_read(ws->conn, ws->in.data + ws->in.len, 4096, IO_MS);
        if (r <= 0) {
            h5_format(error, error_size, "%s stopped answering.", url->host);
            goto fail;
        }
        ws->in.len += (size_t)r;
    }
fail:
    h5_ws_close(ws);
    return -1;
}

int h5_ws_send(H5Ws *ws, const char *text, size_t length)
{
    return ws->conn ? send_frame(ws, 1, text, length) : -1;
}

/* One whole frame from ws->in: 1 (its opcode, fin and payload set, the bytes
 * consumed by *used), 0 when it isn't all there yet, -1 when it's broken. */
static int take_frame(H5Ws *ws, int *opcode, int *fin, const uint8_t **payload, size_t *length, size_t *used)
{
    const uint8_t *p = ws->in.data;
    size_t have = ws->in.len, n = 2;
    uint64_t size;

    if (have < 2) return 0;
    *fin = p[0] >> 7;
    *opcode = p[0] & 15;
    int masked = p[1] >> 7;
    size = p[1] & 127;
    if (size == 126) {
        if (have < 4) return 0;
        size = (uint64_t)p[2] << 8 | p[3];
        n = 4;
    } else if (size == 127) {
        if (have < 10) return 0;
        size = 0;
        for (int i = 0; i < 8; i++) size = size << 8 | p[2 + i];
        n = 10;
    }
    if (size > MESSAGE_MAX) return -1;
    if (masked) n += 4;
    if (have < n + size) return 0;
    if (masked) {
        uint8_t *data = ws->in.data + n, *mask = ws->in.data + n - 4;
        for (uint64_t i = 0; i < size; i++) data[i] ^= mask[i & 3];
    }
    *payload = p + n;
    *length = (size_t)size;
    *used = n + (size_t)size;
    return 1;
}

int h5_ws_poll(H5Ws *ws, int timeout_ms, H5Buf *message)
{
    int first = 1;
    if (!ws->conn) return -1;
    for (;;) {
        int opcode, fin, r;
        const uint8_t *payload;
        size_t length, used;
        while ((r = take_frame(ws, &opcode, &fin, &payload, &length, &used)) == 1) {
            if (opcode == 9) {                       /* ping: pong with the same */
                H5Buf copy = { 0 };
                h5_buf_put(&copy, payload, length);
                h5_buf_consume(&ws->in, used);
                r = send_frame(ws, 10, copy.data, copy.len);
                h5_buf_free(&copy);
                if (r) return -1;
                continue;
            }
            if (opcode == 8) {                       /* close */
                note("H5 net WebSocket closed by the server");
                return -1;
            }
            if (opcode == 1 || (opcode == 0 && ws->fragmented)) {
                if (opcode == 1) ws->message.len = 0;
                if (h5_buf_put(&ws->message, payload, length) || ws->message.len > MESSAGE_MAX) return -1;
                h5_buf_consume(&ws->in, used);
                if (!fin) {
                    ws->fragmented = 1;
                    continue;
                }
                ws->fragmented = 0;
                message->len = 0;
                if (h5_buf_put(message, ws->message.data, ws->message.len) || h5_buf_terminate(message)) return -1;
                ws->message.len = 0;
                return 1;
            }
            h5_buf_consume(&ws->in, used);           /* pong, binary: not for us */
        }
        if (r < 0) return -1;
        /* More bytes: the first read waits as asked; a message on its way gets time to arrive. */
        int wait = first ? timeout_ms : ws->in.len ? IO_MS : 0;
        first = 0;
        if (h5_buf_reserve(&ws->in, READ_CHUNK)) return -1;
        r = h5_conn_read(ws->conn, ws->in.data + ws->in.len, READ_CHUNK, wait);
        if (r == H5_NET_AGAIN) return 0;
        if (r <= 0) return -1;
        ws->in.len += (size_t)r;
    }
}

void h5_ws_close(H5Ws *ws)
{
    if (ws->conn) {
        uint8_t code[2] = { 0x03, 0xE8 };   /* 1000: normal */
        send_frame(ws, 8, code, 2);
        h5_conn_close(ws->conn);
    }
    h5_buf_free(&ws->in);
    h5_buf_free(&ws->message);
    memset(ws, 0, sizeof(*ws));
}
