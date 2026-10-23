/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Home5: drawing into the 1920x1080 frame on the CPU: shapes with soft edges
 * and text from the font atlas tools/make-font.py makes. (Taken from PC5.) */
#ifndef H5_DRAW_H
#define H5_DRAW_H
#include "h5.h"

#include <math.h>

typedef struct H5Canvas {
    uint32_t *px;      /* 0xAARRGGBB, the frame's BGRX */
    int w, h, stride;  /* stride in pixels */
} H5Canvas;

/* ---- text ------------------------------------------------------------------------ */
/* The faces tools/make-font.py renders, in its order. */
typedef enum H5Face {
    H5_FACE_CAPS,     /* 16 semibold: section labels ("ROOMS") */
    H5_FACE_KEY,      /* 17 bold: the letters on button glyphs ("L2", "OPTIONS") */
    H5_FACE_SMALL,    /* 20 regular */
    H5_FACE_BODY,     /* 22 regular */
    H5_FACE_TEXT,     /* 26 regular */
    H5_FACE_LABEL,    /* 26 semibold */
    H5_FACE_CARD,     /* 28 semibold */
    H5_FACE_HEAD,     /* 40 bold */
    H5_FACE_TITLE,    /* 52 bold */
    H5_FACE_BIG,      /* 64 bold */
    H5_FACE_HUGE,     /* 88 bold, numbers */
    H5_FACE_DIAL,     /* 132 bold, numbers */
    H5_FACES
} H5Face;

typedef struct H5Glyph {
    uint32_t codepoint;
    uint16_t x, y, w, h;
    int16_t left, top;
    uint16_t advance;    /* 64ths of a pixel */
} H5Glyph;

typedef struct H5Fonts {
    int ascent[H5_FACES], descent[H5_FACES], line[H5_FACES];
    const H5Glyph *glyphs[H5_FACES];
    uint32_t count[H5_FACES];
    const uint8_t *atlas;
    int atlas_w, atlas_h;
    H5Buf file;
} H5Fonts;

int h5_fonts_load(H5Fonts *fonts, const char *path);
void h5_fonts_free(H5Fonts *fonts);
/* Width of text in pixels. */
int h5_text_width(const H5Fonts *fonts, H5Face face, const char *text);
/* Draws text with its baseline at y, cut with "…" to max_width (0: no
 * limit). align: 0 left of x, 1 centred on x, 2 right of x. Returns the
 * width drawn. */
int h5_text(H5Canvas *c, const H5Fonts *fonts, H5Face face, int x, int y, uint32_t color, const char *text,
             int max_width, int align);
/* Text wrapped into lines of max_width, at most max_lines (the last cut with
 * "…"), each centred (align 1) or left (0). Returns the lines drawn. */
int h5_text_wrap(H5Canvas *c, const H5Fonts *fonts, H5Face face, int x, int y, int max_width, int max_lines,
                  int line_height, uint32_t color, const char *text, int align);

/* cos(a) as sin(a + pi/2): with both sinf and cosf of one angle, Clang would call
 * sincosf, which the console's C library may not have. */
static inline float h5_cos(float a) { return sinf(a + 1.57079633f); }

/* ---- shapes --------------------------------------------------------------------- */
uint32_t h5_rgba(int r, int g, int b, int a);
uint32_t h5_mix(uint32_t a, uint32_t b, int t255);   /* a towards b */
void h5_clear(H5Canvas *c, uint32_t color);
/* A rectangle with rounded corners (radius 0: square), soft-edged. */
void h5_round_rect(H5Canvas *c, float x, float y, float w, float h, float radius, uint32_t color);
void h5_round_frame(H5Canvas *c, float x, float y, float w, float h, float radius, float width, uint32_t color);
void h5_circle(H5Canvas *c, float cx, float cy, float r, uint32_t color);
void h5_ring(H5Canvas *c, float cx, float cy, float r, float width, uint32_t color);
void h5_line(H5Canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t color);
/* A triangle grown by round on every side (its corners rounded). */
void h5_triangle(H5Canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, float round, uint32_t color);
/* An arc from angle a0 to a1 (radians, clockwise from the top). */
void h5_arc(H5Canvas *c, float cx, float cy, float r, float width, float a0, float a1, uint32_t color);
/* A soft shadow (or glow) around a rounded rectangle. */
void h5_shadow(H5Canvas *c, float x, float y, float w, float h, float radius, float spread, uint32_t color);

#endif
