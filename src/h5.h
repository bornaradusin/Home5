/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Home5: Home Assistant on the PS5.
 *
 * What every part of the app shares: limits, memory, byte buffers, images,
 * files, hashes and text. Plain C11 and POSIX, so the same code runs in the
 * host tests and on the console. (Taken from PC5's pc5.h.) */
#ifndef H5_H
#define H5_H
#include <stddef.h>
#include <stdint.h>

#define H5_VERSION "1.0.3"
#ifndef H5_TITLE_ID
#define H5_TITLE_ID "PPSA11019"
#endif
#define H5_REPO "https://github.com/bornaradusin/Home5"

enum {
    H5_PATH = 1024,     /* a path on the console */
    H5_ERROR = 256,
};

/* ---- memory ----------------------------------------------------------------
 * Blocks of 64 KiB and more get pages of their own. */
void *h5_alloc(size_t size);            /* zeroed; NULL when out of memory */
void *h5_realloc(void *block, size_t size);
void h5_free(void *block);

/* ---- byte buffers ----------------------------------------------------------- */
typedef struct H5Buf {
    uint8_t *data;
    size_t len, cap;
} H5Buf;

int h5_buf_reserve(H5Buf *buf, size_t extra);   /* 0, or -1 out of memory */
int h5_buf_put(H5Buf *buf, const void *data, size_t size);
int h5_buf_puts(H5Buf *buf, const char *text);
int h5_buf_printf(H5Buf *buf, const char *format, ...) __attribute__((format(printf, 2, 3)));
/* Ends the text with a 0 byte that len does not count. */
int h5_buf_terminate(H5Buf *buf);
/* Drops the first n bytes. */
void h5_buf_consume(H5Buf *buf, size_t n);
void h5_buf_free(H5Buf *buf);

/* ---- images ------------------------------------------------------------------
 * Pixels are 0xAARRGGBB, which is B, G, R, A in memory. */
typedef struct H5Image {
    int w, h;
    uint32_t *px;
} H5Image;

int h5_image_new(H5Image *image, int w, int h);  /* zeroed; 0 or -1 */
void h5_image_free(H5Image *image);

/* ---- files ---------------------------------------------------------------------- */
int h5_join(char *out, size_t size, const char *a, const char *b);   /* a/b; -1 if too long */
int h5_is_dir(const char *path);
int h5_is_file(const char *path);
int h5_exists(const char *path);                 /* anything, links not followed */
int h5_mkdirs(const char *path);                 /* mkdir -p */
/* The whole file, up to max bytes (more is an error); text gets a 0 byte. */
int h5_read_file(const char *path, H5Buf *out, size_t max);
/* Written through path.part, flushed and renamed into place. */
int h5_write_file(const char *path, const void *data, size_t size);

/* ---- hashes and encodings ----------------------------------------------------------- */
void h5_sha1(const void *data, size_t size, uint8_t digest[20]);
/* Base64 of data into out (0-terminated); -1 when out is too small. */
int h5_base64(const void *data, size_t size, char *out, size_t out_size);
/* text with every byte but A-Z a-z 0-9 - . _ ~ as %XX (form and query values). */
int h5_url_encode(H5Buf *out, const char *text);

/* ---- text ---------------------------------------------------------------------- */
/* text copied into out, cut to fit (always 0-terminated, never in the middle of a
 * UTF-8 character). */
void h5_copy(char *out, size_t size, const char *text);
/* snprintf that reports truncation: 0, or -1 when out was too small. */
int h5_format(char *out, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
/* ASCII lower case of one byte. */
static inline int h5_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int h5_casecmp(const char *a, const char *b);
int h5_casecmp_n(const char *a, const char *b, size_t n);   /* the first n bytes */
/* Spaces and tabs off both ends, in place. */
char *h5_trim(char *text);
/* NAN (an unknown number); the console's isnan macro is noisy under Clang 18. */
static inline int h5_isnan(float x) { return x != x; }

#endif
