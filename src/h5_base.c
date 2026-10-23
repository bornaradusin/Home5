/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Home5: memory, buffers, images, files, SHA-256 and text (pc5.h). */
#include "h5.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- memory ------------------------------------------------------------------ */

enum { H5_MAPPED_FROM = 64 * 1024, H5_HEADER = 16 };

typedef struct H5Block {
    size_t size;      /* what was asked for */
    size_t mapped;    /* bytes mapped, 0 for the heap */
} H5Block;

void *h5_alloc(size_t size)
{
    H5Block *block;

    if (size > SIZE_MAX / 2) return NULL;
#ifndef H5_HEAP_ONLY   /* the host tests keep everything on the heap, where the sanitizers see it */
    if (size + H5_HEADER >= H5_MAPPED_FROM) {
        size_t page = 16384, length = (size + H5_HEADER + page - 1) / page * page;
        void *pages = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (pages == MAP_FAILED) return NULL;
        block = pages;
        block->mapped = length;
    } else
#endif
    {
        block = calloc(1, size + H5_HEADER);
        if (!block) return NULL;
        block->mapped = 0;
    }
    block->size = size;
    return (uint8_t *)block + H5_HEADER;
}

void h5_free(void *pointer)
{
    H5Block *block;

    if (!pointer) return;
    block = (H5Block *)((uint8_t *)pointer - H5_HEADER);
    if (block->mapped) munmap(block, block->mapped);
    else free(block);
}

void *h5_realloc(void *pointer, size_t size)
{
    H5Block *block;
    void *grown;

    if (!pointer) return h5_alloc(size);
    block = (H5Block *)((uint8_t *)pointer - H5_HEADER);
    if (block->mapped && size + H5_HEADER <= block->mapped) {
        block->size = size;
        return pointer;
    }
    if (!(grown = h5_alloc(size))) return NULL;
    memcpy(grown, pointer, block->size < size ? block->size : size);
    h5_free(pointer);
    return grown;
}

/* ---- buffers ------------------------------------------------------------------- */

int h5_buf_reserve(H5Buf *buf, size_t extra)
{
    size_t wanted, cap;
    uint8_t *data;

    if (extra > SIZE_MAX / 4 - buf->len) return -1;
    wanted = buf->len + extra + 1;   /* room for a closing 0 byte */
    if (wanted <= buf->cap) return 0;
    cap = buf->cap ? buf->cap : 256;
    while (cap < wanted) cap *= 2;
    if (!(data = h5_realloc(buf->data, cap))) return -1;
    buf->data = data;
    buf->cap = cap;
    return 0;
}

int h5_buf_put(H5Buf *buf, const void *data, size_t size)
{
    if (h5_buf_reserve(buf, size)) return -1;
    if (size) memcpy(buf->data + buf->len, data, size);
    buf->len += size;
    buf->data[buf->len] = 0;
    return 0;
}

int h5_buf_puts(H5Buf *buf, const char *text)
{
    return h5_buf_put(buf, text, strlen(text));
}

int h5_buf_printf(H5Buf *buf, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (length < 0 || h5_buf_reserve(buf, (size_t)length)) return -1;
    va_start(args, format);
    vsnprintf((char *)buf->data + buf->len, (size_t)length + 1, format, args);
    va_end(args);
    buf->len += (size_t)length;
    return 0;
}

int h5_buf_terminate(H5Buf *buf)
{
    if (h5_buf_reserve(buf, 0)) return -1;
    buf->data[buf->len] = 0;
    return 0;
}

void h5_buf_consume(H5Buf *buf, size_t n)
{
    if (n >= buf->len) {
        buf->len = 0;
        return;
    }
    memmove(buf->data, buf->data + n, buf->len - n);
    buf->len -= n;
}

void h5_buf_free(H5Buf *buf)
{
    h5_free(buf->data);
    *buf = (H5Buf){ 0 };
}

/* ---- images --------------------------------------------------------------------- */

int h5_image_new(H5Image *image, int w, int h)
{
    *image = (H5Image){ 0 };
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;
    if (!(image->px = h5_alloc((size_t)w * (size_t)h * 4u))) return -1;
    image->w = w;
    image->h = h;
    return 0;
}

void h5_image_free(H5Image *image)
{
    h5_free(image->px);
    *image = (H5Image){ 0 };
}

/* ---- files ---------------------------------------------------------------------- */

int h5_join(char *out, size_t size, const char *a, const char *b)
{
    int length = snprintf(out, size, "%s/%s", a, b);
    return length < 0 || (size_t)length >= size ? -1 : 0;
}

int h5_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int h5_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int h5_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int make_dir(const char *path)
{
    return mkdir(path, 0777) == 0 || (errno == EEXIST && h5_is_dir(path)) ? 0 : -1;
}

int h5_mkdirs(const char *path)
{
    char folder[H5_PATH];

    if (h5_format(folder, sizeof(folder), "%s", path)) return -1;
    for (char *slash = strchr(folder + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        if (make_dir(folder)) return -1;
        *slash = '/';
    }
    return make_dir(folder);
}

int h5_read_file(const char *path, H5Buf *out, size_t max)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    ssize_t got;

    out->len = 0;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > max ||
        h5_buf_reserve(out, (size_t)st.st_size)) {
        close(fd);
        return -1;
    }
    while ((got = read(fd, out->data + out->len, out->cap - 1 - out->len)) > 0) {
        out->len += (size_t)got;
        if (out->len + 1 == out->cap) {
            if (out->len >= max || h5_buf_reserve(out, 4096)) { got = -1; break; }
        }
    }
    close(fd);
    if (got < 0 || out->len > max) return -1;
    out->data[out->len] = 0;
    return 0;
}

int h5_write_file(const char *path, const void *data, size_t size)
{
    char part[H5_PATH + 8];
    const uint8_t *at = data;
    int fd, ok = 1;

    if (h5_format(part, sizeof(part), "%s.part", path)) return -1;
    if ((fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return -1;
    for (size_t done = 0; ok && done < size;) {
        ssize_t put = write(fd, at + done, size - done);
        if (put <= 0) ok = 0;
        else done += (size_t)put;
    }
    if (ok && fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (!ok || rename(part, path)) {
        int saved = errno;
        unlink(part);
        errno = saved;
        return -1;
    }
    return 0;
}

/* ---- hashes and encodings ------------------------------------------------------------ */

/* SHA-1 (FIPS 180-4): only for the WebSocket handshake's check. */
static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void h5_sha1(const void *data, size_t size, uint8_t digest[20])
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    const uint8_t *in = data;
    uint64_t bits = (uint64_t)size * 8;
    size_t total = (size + 9 + 63) / 64 * 64;

    for (size_t offset = 0; offset < total; offset += 64) {
        uint8_t block[64];
        uint32_t w[80];
        for (size_t i = 0; i < 64; i++) {
            size_t at = offset + i;
            block[i] = at < size ? in[at] : at == size ? 0x80 : 0;
        }
        if (offset + 64 == total)
            for (int i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 |
                   block[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) digest[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

int h5_base64(const void *data, size_t size, char *out, size_t out_size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *in = data;
    size_t need = (size + 2) / 3 * 4 + 1, o = 0;

    if (out_size < need) return -1;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < size ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < size ? in[i + 2] : 0);
        out[o++] = alphabet[v >> 18 & 63];
        out[o++] = alphabet[v >> 12 & 63];
        out[o++] = i + 1 < size ? alphabet[v >> 6 & 63] : '=';
        out[o++] = i + 2 < size ? alphabet[v & 63] : '=';
    }
    out[o] = 0;
    return 0;
}

int h5_url_encode(H5Buf *out, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~') {
            if (h5_buf_put(out, p, 1)) return -1;
        } else {
            char esc[3] = { '%', hex[*p >> 4], hex[*p & 15] };
            if (h5_buf_put(out, esc, 3)) return -1;
        }
    }
    return 0;
}

/* ---- text ----------------------------------------------------------------------- */

void h5_copy(char *out, size_t size, const char *text)
{
    size_t n = 0;

    if (!size) return;
    while (text && text[n] && n + 1 < size) n++;
    /* Not in the middle of a character. */
    if (text && text[n]) while (n && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    memmove(out, text ? text : "", n);
    out[n] = 0;
}

int h5_format(char *out, size_t size, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(out, size, format, args);
    va_end(args);
    return length < 0 || (size_t)length >= size ? -1 : 0;
}

int h5_casecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = h5_lower((unsigned char)*a), y = h5_lower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int h5_casecmp_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = h5_lower((unsigned char)a[i]), cb = h5_lower((unsigned char)b[i]);
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

char *h5_trim(char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) text[--n] = 0;
    return text;
}
