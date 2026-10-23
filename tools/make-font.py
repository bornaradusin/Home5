#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Renders Home5's text faces from the Inter fonts (fonts/, SIL OFL 1.1) into one atlas.
(Taken from PC5.)

    tools/make-font.py OUT.bin

The console draws text from this file (src/h5_draw.c), so it needs no font engine.
Layout, little-endian:

    "H5F1" u32 version (1) u32 faces u32 atlas_width u32 atlas_height
    per face:  u32 pixel_size i32 ascent i32 descent i32 line_height u32 glyphs u32 first_glyph
    per glyph: u32 codepoint u16 x u16 y u16 w u16 h i16 left i16 top u16 advance_64ths u16 0
    the atlas: atlas_width * atlas_height bytes of coverage (0-255)

Glyphs are sorted by code point within each face; top is from the baseline (negative is up).
"""
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONTS = Path(__file__).resolve().parents[1] / "fonts"
TEXT = sorted(set(range(0x20, 0x7F)) | set(range(0xA0, 0x180)) |
              {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2122, 0x2190, 0x2192,
               0x20AC, 0x0218, 0x0219, 0x021A, 0x021B, 0x203A, 0x2713})
NUMBERS = sorted(set(map(ord, "0123456789.,-+%° CFKW")))
# name, file, pixel size, code points: the faces src/h5_draw.h names, in its order.
FACES = [
    ("caps", "Inter-SemiBold.ttf", 16, TEXT),
    ("key", "Inter-SemiBold.ttf", 17, TEXT),
    ("small", "Inter-Regular.ttf", 20, TEXT),
    ("body", "Inter-Regular.ttf", 22, TEXT),
    ("text", "Inter-Regular.ttf", 26, TEXT),
    ("label", "Inter-SemiBold.ttf", 26, TEXT),
    ("card", "Inter-SemiBold.ttf", 28, TEXT),
    ("head", "InterDisplay-Bold.ttf", 40, TEXT),
    ("title", "InterDisplay-Bold.ttf", 52, TEXT),
    ("big", "InterDisplay-Bold.ttf", 64, TEXT),
    ("huge", "InterDisplay-Bold.ttf", 88, NUMBERS),
    ("dial", "InterDisplay-Bold.ttf", 132, NUMBERS),
]
ATLAS_WIDTH = 2048


def main() -> int:
    out = Path(sys.argv[1])
    faces, glyphs, bitmaps = [], [], []
    for name, file, size, codepoints in FACES:
        font = ImageFont.truetype(str(FONTS / file), size)
        ascent, descent = font.getmetrics()
        first = len(glyphs)
        for cp in codepoints:
            ch = chr(cp)
            # Only what the font really has: a missing glyph is left out, not drawn as a box.
            if cp != 0x20 and font.getmask(ch).getbbox() is None and not ch.isspace():
                continue
            advance = font.getlength(ch)
            x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
            w, h = max(0, x1 - x0), max(0, y1 - y0)
            image = Image.new("L", (max(w, 1), max(h, 1)), 0)
            if w and h:
                ImageDraw.Draw(image).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            glyphs.append([cp, 0, 0, w, h, x0, y0, int(round(advance * 64))])
            bitmaps.append(image)
        faces.append((size, ascent, descent, int(round((ascent + descent) * 1.18)), len(glyphs) - first, first))
    # Shelf packing, tallest first.
    order = sorted(range(len(glyphs)), key=lambda i: -glyphs[i][4])
    x = y = shelf = 0
    for i in order:
        w, h = glyphs[i][3], glyphs[i][4]
        if not w or not h:
            continue
        if x + w + 1 > ATLAS_WIDTH:
            x, y, shelf = 0, y + shelf + 1, 0
        glyphs[i][1], glyphs[i][2] = x, y
        x += w + 1
        shelf = max(shelf, h)
    height = y + shelf + 1
    atlas = Image.new("L", (ATLAS_WIDTH, height), 0)
    for i, glyph in enumerate(glyphs):
        if glyph[3] and glyph[4]:
            atlas.paste(bitmaps[i], (glyph[1], glyph[2]))
    data = bytearray(b"H5F1" + struct.pack("<IIII", 1, len(faces), ATLAS_WIDTH, height))
    for face in faces:
        data += struct.pack("<IiiiII", *face)
    for cp, gx, gy, w, h, left, top, advance in glyphs:
        data += struct.pack("<IHHHHhhHH", cp, gx, gy, w, h, left, top, advance, 0)
    data += atlas.tobytes()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(data))
    print(f"make-font: {out}: {len(faces)} faces, {len(glyphs)} glyphs, {ATLAS_WIDTH}x{height} atlas, {len(data) >> 10} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
