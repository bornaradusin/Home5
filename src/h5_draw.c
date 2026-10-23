/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Home5: drawing (h5_draw.h). Shapes are drawn from their distance to each
 * pixel, which gives soft edges without any oversampling. */
#include "h5_draw.h"

#include <math.h>
#include <string.h>

/* ---- colours ------------------------------------------------------------------------- */

uint32_t h5_rgba(int r, int g, int b, int a)
{
    return (uint32_t)(a & 255) << 24 | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
}

uint32_t h5_mix(uint32_t a, uint32_t b, int t)
{
    uint32_t out = 0;
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    for (int k = 0; k < 32; k += 8) {
        int x = (int)((a >> k) & 255), y = (int)((b >> k) & 255);
        out |= (uint32_t)(x + (y - x) * t / 255) << k;
    }
    return out;
}

/* dst blended towards color by color's alpha times cover (0-256). */
static inline void blend(uint32_t *dst, uint32_t color, int cover)
{
    int a = (int)(color >> 24) * cover >> 8;
    if (a <= 0) return;
    if (a >= 255) { *dst = color | 0xff000000u; return; }
    uint32_t d = *dst;
    uint32_t rb = ((color & 0xff00ffu) * (uint32_t)a + (d & 0xff00ffu) * (uint32_t)(255 - a)) >> 8 & 0xff00ffu;
    uint32_t g = ((color & 0x00ff00u) * (uint32_t)a + (d & 0x00ff00u) * (uint32_t)(255 - a)) >> 8 & 0x00ff00u;
    *dst = 0xff000000u | rb | g;
}

void h5_clear(H5Canvas *c, uint32_t color)
{
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = c->px + (size_t)y * (size_t)c->stride;
        for (int x = 0; x < c->w; x++) row[x] = color | 0xff000000u;
    }
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- shapes --------------------------------------------------------------------------- */

/* Signed distance from (px, py) to a rounded rectangle given by its centre,
 * half sizes and radius: negative inside. */
static inline float rect_distance(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

static void rect_shape(H5Canvas *c, float x, float y, float w, float h, float radius, float width, uint32_t color)
{
    float hw = w / 2, hh = h / 2, cx = x + hw, cy = y + hh;
    int x0, x1, y0, y1;

    if (w <= 0 || h <= 0 || !(color >> 24)) return;
    if (radius > hw) radius = hw;
    if (radius > hh) radius = hh;
    x0 = (int)floorf(x - 1); x1 = (int)ceilf(x + w + 1);
    y0 = (int)floorf(y - 1); y1 = (int)ceilf(y + h + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        float fy = py + 0.5f;
        /* The span sure to be inside (filled) or, for a frame, inside the hole. */
        float dy = fabsf(fy - cy) - (hh - radius);
        float half = dy <= 0 ? hw : (dy < radius ? (hw - radius) + sqrtf(radius * radius - dy * dy) : 0);
        int in0 = (int)ceilf(cx - half + 1.5f), in1 = (int)floorf(cx + half - 1.5f);
        if (width > 0) {
            float hole = half - width;
            in0 = (int)ceilf(cx - hole + 1.5f);
            in1 = (int)floorf(cx + hole - 1.5f);
            if (fabsf(fy - cy) > hh - width - 1.5f) in0 = 1, in1 = 0;
        }
        for (int px = x0; px < x1; px++) {
            if (px >= in0 && px <= in1) {
                if (width > 0) { px = in1; continue; }
                blend(&row[px], color, 256);
                continue;
            }
            float d = rect_distance(px + 0.5f, fy, cx, cy, hw, hh, radius);
            if (width > 0) d = fabsf(d + width / 2) - width / 2;
            float cover = clampf(0.5f - d, 0, 1);
            if (cover > 0) blend(&row[px], color, (int)(cover * 256));
        }
    }
}

void h5_round_rect(H5Canvas *c, float x, float y, float w, float h, float radius, uint32_t color)
{
    rect_shape(c, x, y, w, h, radius, 0, color);
}

void h5_round_frame(H5Canvas *c, float x, float y, float w, float h, float radius, float width, uint32_t color)
{
    rect_shape(c, x, y, w, h, radius, width, color);
}

/* Any shape given by a distance function over a box. */
typedef float (*DistanceFn)(float px, float py, const float *p);

static void shape(H5Canvas *c, float bx0, float by0, float bx1, float by1, DistanceFn fn, const float *p, uint32_t color)
{
    int x0 = (int)floorf(bx0), y0 = (int)floorf(by0), x1 = (int)ceilf(bx1), y1 = (int)ceilf(by1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        for (int px = x0; px < x1; px++) {
            float cover = clampf(0.5f - fn(px + 0.5f, py + 0.5f, p), 0, 1);
            if (cover > 0) blend(&row[px], color, (int)(cover * 256));
        }
    }
}

static float circle_fn(float px, float py, const float *p)
{
    return sqrtf((px - p[0]) * (px - p[0]) + (py - p[1]) * (py - p[1])) - p[2];
}

static float ring_fn(float px, float py, const float *p)
{
    return fabsf(circle_fn(px, py, p)) - p[3] / 2;
}

static float line_fn(float px, float py, const float *p)
{
    float ax = px - p[0], ay = py - p[1], bx = p[2] - p[0], by = p[3] - p[1];
    float t = clampf((ax * bx + ay * by) / (bx * bx + by * by + 1e-6f), 0, 1);
    float dx = ax - bx * t, dy = ay - by * t;
    return sqrtf(dx * dx + dy * dy) - p[4] / 2;
}

static float arc_fn(float px, float py, const float *p)
{
    float angle = atan2f(px - p[0], -(py - p[1]));   /* clockwise from the top */
    if (angle < 0) angle += 6.2831853f;
    float a0 = p[4], a1 = p[5];
    int inside = a1 - a0 >= 6.283f || (angle >= a0 && angle <= a1) || (angle + 6.2831853f >= a0 && angle + 6.2831853f <= a1);
    if (inside) return fabsf(circle_fn(px, py, p)) - p[3] / 2;
    /* the round ends */
    float d = 1e9f;
    for (int k = 0; k < 2; k++) {
        float a = k ? a1 : a0, ex = p[0] + sinf(a) * p[2], ey = p[1] - h5_cos(a) * p[2];
        float e = sqrtf((px - ex) * (px - ex) + (py - ey) * (py - ey)) - p[3] / 2;
        if (e < d) d = e;
    }
    return d;
}

void h5_circle(H5Canvas *c, float cx, float cy, float r, uint32_t color)
{
    float p[3] = { cx, cy, r };
    shape(c, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, circle_fn, p, color);
}

void h5_ring(H5Canvas *c, float cx, float cy, float r, float width, uint32_t color)
{
    float p[4] = { cx, cy, r, width }, e = r + width + 1;
    shape(c, cx - e, cy - e, cx + e, cy + e, ring_fn, p, color);
}

void h5_line(H5Canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t color)
{
    float p[5] = { x0, y0, x1, y1, width }, e = width + 1;
    shape(c, fminf(x0, x1) - e, fminf(y0, y1) - e, fmaxf(x0, x1) + e, fmaxf(y0, y1) + e, line_fn, p, color);
}

void h5_arc(H5Canvas *c, float cx, float cy, float r, float width, float a0, float a1, uint32_t color)
{
    while (a0 < 0) { a0 += 6.2831853f; a1 += 6.2831853f; }
    while (a0 >= 6.2831853f) { a0 -= 6.2831853f; a1 -= 6.2831853f; }
    float p[6] = { cx, cy, r, width, a0, a1 }, e = r + width + 1;
    shape(c, cx - e, cy - e, cx + e, cy + e, arc_fn, p, color);
}

/* Signed distance to a triangle (after Inigo Quilez). */
static float triangle_fn(float px, float py, const float *p)
{
    float d = 1e9f, s = 1;
    for (int i = 0, j = 2; i < 3; j = i++) {
        float ex = p[2 * j] - p[2 * i], ey = p[2 * j + 1] - p[2 * i + 1];
        float wx = px - p[2 * i], wy = py - p[2 * i + 1];
        float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey + 1e-6f), 0, 1);
        float bx = wx - ex * t, by = wy - ey * t;
        d = fminf(d, bx * bx + by * by);
        int c1 = py >= p[2 * i + 1], c2 = py < p[2 * j + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * sqrtf(d) - p[6];
}

void h5_triangle(H5Canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, float round, uint32_t color)
{
    float p[7] = { x0, y0, x1, y1, x2, y2, round }, e = round + 1;
    shape(c, fminf(x0, fminf(x1, x2)) - e, fminf(y0, fminf(y1, y2)) - e, fmaxf(x0, fmaxf(x1, x2)) + e,
          fmaxf(y0, fmaxf(y1, y2)) + e, triangle_fn, p, color);
}

void h5_shadow(H5Canvas *c, float x, float y, float w, float h, float radius, float spread, uint32_t color)
{
    float hw = w / 2, hh = h / 2, cx = x + hw, cy = y + hh;
    int x0 = (int)floorf(x - spread), y0 = (int)floorf(y - spread), x1 = (int)ceilf(x + w + spread),
        y1 = (int)ceilf(y + h + spread);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * (size_t)c->stride;
        /* Under the shape itself nothing shows: those columns are skipped. */
        int skip0 = 1, skip1 = 0;
        if (fabsf(py + 0.5f - cy) < hh - radius - 2) {
            skip0 = (int)ceilf(cx - hw + 2);
            skip1 = (int)floorf(cx + hw - 2);
        }
        for (int px = x0; px < x1; px++) {
            if (px >= skip0 && px < skip1) { px = skip1 - 1; continue; }
            float d = rect_distance(px + 0.5f, py + 0.5f, cx, cy, hw, hh, radius);
            if (d < -2) continue;
            float t = clampf(1 - d / spread, 0, 1);
            if (t > 0) blend(&row[px], color, (int)(t * t * 256));
        }
    }
}

/* ---- pictures --------------------------------------------------------------------------- */

/* ---- text ------------------------------------------------------------------------------- */

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int h5_fonts_load(H5Fonts *fonts, const char *path)
{
    const uint8_t *d;
    uint32_t faces, glyph_total = 0;
    size_t at;

    memset(fonts, 0, sizeof(*fonts));
    if (h5_read_file(path, &fonts->file, 32u * 1024 * 1024) || fonts->file.len < 20) goto bad;
    d = fonts->file.data;
    if (memcmp(d, "H5F1", 4) || get32(d + 4) != 1) goto bad;
    faces = get32(d + 8);
    fonts->atlas_w = (int)get32(d + 12);
    fonts->atlas_h = (int)get32(d + 16);
    if (faces != H5_FACES) goto bad;
    at = 20;
    for (uint32_t f = 0; f < faces; f++, at += 24) {
        if (at + 24 > fonts->file.len) goto bad;
        fonts->ascent[f] = (int)get32(d + at + 4);
        fonts->descent[f] = (int)get32(d + at + 8);
        fonts->line[f] = (int)get32(d + at + 12);
        fonts->count[f] = get32(d + at + 16);
        glyph_total += fonts->count[f];
    }
    /* Glyphs are 20 bytes each, laid out as H5Glyph. */
    if (sizeof(H5Glyph) != 20 || at + (size_t)glyph_total * 20 + (size_t)fonts->atlas_w * (size_t)fonts->atlas_h > fonts->file.len)
        goto bad;
    {
        size_t face_at = 20, first;
        for (uint32_t f = 0; f < faces; f++, face_at += 24) {
            first = get32(d + face_at + 20);
            fonts->glyphs[f] = (const H5Glyph *)(d + at + first * 20);
        }
    }
    fonts->atlas = d + at + (size_t)glyph_total * 20;
    return 0;
bad:
    h5_fonts_free(fonts);
    return -1;
}

void h5_fonts_free(H5Fonts *fonts)
{
    h5_buf_free(&fonts->file);
    memset(fonts, 0, sizeof(*fonts));
}

static uint32_t next_codepoint(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t c = *p;
    int extra = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;

    if (c >= 0x80) c &= extra == 1 ? 0x1f : extra == 2 ? 0x0f : 0x07;
    p++;
    for (int k = 0; k < extra && (*p & 0xc0) == 0x80; k++) c = c << 6 | (*p++ & 0x3f);
    *text = (const char *)p;
    return c;
}

static const H5Glyph *glyph(const H5Fonts *fonts, H5Face face, uint32_t codepoint)
{
    const H5Glyph *g = fonts->glyphs[face];
    int lo = 0, hi = (int)fonts->count[face] - 1;

    if (!g) return NULL;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g[mid].codepoint == codepoint) return &g[mid];
        if (g[mid].codepoint < codepoint) lo = mid + 1;
        else hi = mid - 1;
    }
    return codepoint != '?' ? glyph(fonts, face, '?') : NULL;
}

/* Width in 64ths of a pixel of text's first `bytes` bytes. */
static int width_64(const H5Fonts *fonts, H5Face face, const char *text, size_t bytes)
{
    const char *end = text + bytes;
    int width = 0;

    while (text < end && *text) {
        const H5Glyph *g = glyph(fonts, face, next_codepoint(&text));
        if (g) width += g->advance;
    }
    return width;
}

int h5_text_width(const H5Fonts *fonts, H5Face face, const char *text)
{
    return (width_64(fonts, face, text, strlen(text)) + 63) / 64;
}

static void draw_run(H5Canvas *c, const H5Fonts *fonts, H5Face face, int x, int y, uint32_t color, const char *text,
                     size_t bytes)
{
    const char *end = text + bytes;
    int pen = x * 64;

    while (text < end && *text) {
        const H5Glyph *g = glyph(fonts, face, next_codepoint(&text));
        if (!g) continue;
        int gx = (pen + 32) / 64 + g->left, gy = y + g->top;
        for (int row = 0; row < g->h; row++) {
            int py = gy + row;
            if (py < 0 || py >= c->h) continue;
            const uint8_t *src = fonts->atlas + (size_t)(g->y + row) * (size_t)fonts->atlas_w + g->x;
            uint32_t *dst = c->px + (size_t)py * (size_t)c->stride;
            for (int col = 0; col < g->w; col++) {
                int px = gx + col;
                if (px < 0 || px >= c->w || !src[col]) continue;
                blend(&dst[px], color, src[col] + (src[col] >> 7));
            }
        }
        pen += g->advance;
    }
}

int h5_text(H5Canvas *c, const H5Fonts *fonts, H5Face face, int x, int y, uint32_t color, const char *text,
             int max_width, int align)
{
    static const char ellipsis[] = "\xe2\x80\xa6";
    size_t bytes = strlen(text);
    int width = (width_64(fonts, face, text, bytes) + 63) / 64, cut = 0;

    if (!fonts->atlas) return 0;
    if (max_width > 0 && width > max_width) {
        int room = max_width * 64 - width_64(fonts, face, ellipsis, 3);
        while (bytes > 0) {
            do bytes--; while (bytes > 0 && ((unsigned char)text[bytes] & 0xc0) == 0x80);
            if (width_64(fonts, face, text, bytes) <= room) break;
        }
        while (bytes > 0 && text[bytes - 1] == ' ') bytes--;
        width = (width_64(fonts, face, text, bytes) + width_64(fonts, face, ellipsis, 3) + 63) / 64;
        cut = 1;
    }
    if (align == 1) x -= width / 2;
    else if (align == 2) x -= width;
    draw_run(c, fonts, face, x, y, color, text, bytes);
    if (cut) draw_run(c, fonts, face, x + (width_64(fonts, face, text, bytes) + 32) / 64, y, color, ellipsis, 3);
    return width;
}

int h5_text_wrap(H5Canvas *c, const H5Fonts *fonts, H5Face face, int x, int y, int max_width, int max_lines,
                  int line_height, uint32_t color, const char *text, int align)
{
    int lines = 0;

    while (*text && lines < max_lines) {
        size_t take = 0, fit = 0, length = strlen(text);
        while (*text == ' ') { text++; length--; }
        /* As many words as fit. */
        while (take < length) {
            size_t next = take;
            while (next < length && text[next] == ' ') next++;
            while (next < length && text[next] != ' ') next++;
            if (width_64(fonts, face, text, next) > max_width * 64 && fit) break;
            fit = next;
            take = next;
            if (width_64(fonts, face, text, next) > max_width * 64) break;
        }
        if (!fit) fit = length;
        char line[512];
        size_t n = fit < sizeof(line) - 1 ? fit : sizeof(line) - 1;
        if (lines + 1 == max_lines && fit < length) n = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
        memcpy(line, text, n);
        line[n] = 0;
        int lx = align == 1 ? x + max_width / 2 : x;
        h5_text(c, fonts, face, lx, y + lines * line_height, color, line, max_width, align);
        text += fit;
        lines++;
    }
    return lines;
}
