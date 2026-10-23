#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws Home5's artwork for the PS5's home screen.

    art/make-art.py OUT_DIR [PREVIEW_DIR]

    OUT_DIR/icon0.png           the tile: a white house sending a signal from its roof, on black (512x512)
    OUT_DIR/pic0.dds, pic1.dds  the background when Home5 is selected and while it starts (3840x2160, BC7)
    PREVIEW_DIR/background.png  the background as the console decodes it, to look at

Drawn at a larger size and scaled down, for smooth edges. No lettering. The
backgrounds are compressed here (BC7, mode 6), so no texture tools are needed;
they are decoded again and compared with the picture before they are written.
"""
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SCALE = 4
WHITE = (238, 242, 248)
GREY = (169, 180, 198)


def mark(d: ImageDraw.ImageDraw, ox: float, oy: float, s: float, body, signal) -> None:
    """The house and its two arcs as the approved design has them (a 512-unit square), at (ox, oy), s pixels a unit."""
    def p(x, y):
        return (ox + x * s, oy + y * s)
    # The house: roof and body, a 20-unit outline in the same colour with round joins, the door cut out.
    house = [(256, 214), (120, 318), (152, 318), (152, 428), (360, 428), (360, 318), (392, 318)]
    d.polygon([p(x, y) for x, y in house], fill=body)
    d.line([p(x, y) for x, y in house + [house[0]]], fill=body, width=round(20 * s), joint="curve")
    for x, y in house:
        d.ellipse([*p(x - 10, y - 10), *p(x + 10, y + 10)], fill=body)
    d.rounded_rectangle([*p(228, 358), *p(284, 438)], radius=10 * s, fill=(0, 0, 0, 0) if len(body) == 4 else (0, 0, 0))
    # The signal: arcs of radius 64 and 118 around the roof's peak, from 225 to 315 degrees, 26 units wide
    # (centred on the radius), with round ends.
    for radius, colour in ((64, signal[0]), (118, signal[1])):
        outer = radius + 13
        d.arc([*p(256 - outer, 214 - outer), *p(256 + outer, 214 + outer)], start=225, end=315, fill=colour,
              width=round(26 * s))
        for angle in (225, 315):
            cx = 256 + radius * math.cos(math.radians(angle))
            cy = 214 + radius * math.sin(math.radians(angle))
            d.ellipse([*p(cx - 13, cy - 13), *p(cx + 13, cy + 13)], fill=colour)


def icon() -> Image.Image:
    image = Image.new("RGB", (512 * SCALE, 512 * SCALE), (0, 0, 0))
    mark(ImageDraw.Draw(image), 0, 0, SCALE, WHITE, (WHITE, GREY))
    return image.resize((512, 512), Image.LANCZOS)


def background() -> np.ndarray:
    """The app's night blue, lighter to the right, with the mark large and faint there in a soft blue glow."""
    w, h = 3840, 2160
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    base = np.array([11, 15, 23], np.float32)
    lift = np.array([18, 25, 40], np.float32)
    t = np.clip((x / w) * 0.9 + (1 - y / h) * 0.2, 0, 1)[..., None]
    rgb = base + (lift - base) * t
    # The glow behind the mark (the accent, #6ea0ff, very low).
    cx, cy = w * 0.74, h * 0.46
    glow = np.exp(-(((x - cx) / (w * 0.22)) ** 2 + ((y - cy) / (h * 0.36)) ** 2))[..., None]
    rgb += np.array([110, 160, 255], np.float32) * 0.09 * glow
    # The mark: drawn at twice the size, blurred a little at its edges, laid over.
    s2 = 2
    layer = Image.new("RGBA", (w * s2 // 2, h * s2 // 2), (0, 0, 0, 0))
    size = 1500 * s2 // 2
    mark(ImageDraw.Draw(layer), cx * s2 // 2 - size / 2, cy * s2 // 2 - size / 2 - 40, size / 512,
         (40, 52, 76, 255), ((52, 70, 104, 255), (36, 48, 72, 255)))
    layer = layer.filter(ImageFilter.GaussianBlur(1.2))
    over = np.asarray(layer, np.float32)
    a = over[..., 3:4] / 255
    rgb = rgb * (1 - a) + over[..., :3] * a
    # A little noise against banding.
    rgb += np.random.default_rng(5).uniform(-0.6, 0.6, rgb.shape).astype(np.float32)
    return np.clip(np.rint(rgb), 0, 255).astype(np.uint8)


# ---- BC7 (mode 6: two RGBA endpoints of 7 bits and a shared low bit each, 16 levels between) ----
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


def bc7_encode(rgb: np.ndarray) -> bytes:
    h, w, _ = rgb.shape
    blocks = rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3).astype(np.int32)
    # Endpoints: the block's darkest and brightest in each channel, odd (the low bit 1, so alpha is 255).
    lo = blocks.min(axis=1)
    hi = blocks.max(axis=1)
    lo = np.where(lo % 2 == 1, lo, np.maximum(lo - 1, 1))
    hi = np.where(hi % 2 == 1, hi, np.minimum(hi + 1, 255))
    d = (hi - lo).astype(np.float64)
    dd = (d * d).sum(axis=1)
    t = ((blocks - lo[:, None, :]) * d[:, None, :]).sum(axis=2) / np.where(dd == 0, 1, dd)[:, None]
    index = np.abs(np.clip(t, 0, 1)[..., None] * 64 - WEIGHTS[None, None, :]).argmin(axis=2)
    index[dd == 0] = 0
    # The first pixel's index has only 3 bits: swap the ends where it's 8 or more.
    swap = index[:, 0] >= 8
    lo[swap], hi[swap] = hi[swap].copy(), lo[swap].copy()
    index[swap] = 15 - index[swap]
    e0, e1 = lo >> 1, hi >> 1
    n = len(blocks)
    words = [np.zeros(n, np.uint64), np.zeros(n, np.uint64)]

    def put(bit: int, count: int, value) -> None:
        value = np.asarray(value, np.uint64)
        for k in range(count):
            b = bit + k
            words[b // 64] |= ((value >> np.uint64(k)) & np.uint64(1)) << np.uint64(b % 64)

    put(6, 1, 1)                                   # mode 6
    bit = 7
    for channel in range(3):
        put(bit, 7, e0[:, channel]); bit += 7
        put(bit, 7, e1[:, channel]); bit += 7
    put(bit, 7, 127); bit += 7                      # alpha
    put(bit, 7, 127); bit += 7
    put(bit, 1, 1); put(bit + 1, 1, 1); bit += 2   # the low bits
    put(bit, 3, index[:, 0]); bit += 3
    for i in range(1, 16):
        put(bit, 4, index[:, i]); bit += 4
    assert bit == 128
    return np.stack(words, axis=1).astype("<u8").tobytes()


def bc7_decode(data: bytes, w: int, h: int) -> np.ndarray:
    """Mode 6 only, as written above."""
    words = np.frombuffer(data, "<u8").reshape(-1, 2)

    def get(bit: int, count: int) -> np.ndarray:
        out = np.zeros(len(words), np.int32)
        for k in range(count):
            b = bit + k
            out |= (((words[:, b // 64] >> np.uint64(b % 64)) & np.uint64(1)).astype(np.int32)) << k
        return out

    assert (get(0, 7) == 64).all(), "not mode 6"
    bit, e = 7, [[], []]
    for _ in range(3):
        e[0].append(get(bit, 7)); bit += 7
        e[1].append(get(bit, 7)); bit += 7
    bit += 14
    p0, p1 = get(bit, 1), get(bit + 1, 1); bit += 2
    c0 = np.stack([(v << 1) | p0 for v in e[0]], axis=1)
    c1 = np.stack([(v << 1) | p1 for v in e[1]], axis=1)
    index = [get(bit, 3)]; bit += 3
    for _ in range(15):
        index.append(get(bit, 4)); bit += 4
    wts = WEIGHTS[np.stack(index, axis=1)]
    px = ((64 - wts)[..., None] * c0[:, None, :] + wts[..., None] * c1[:, None, :] + 32) >> 6
    return px.reshape(h // 4, w // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3).astype(np.uint8)


def dds(data: bytes, w: int, h: int) -> bytes:
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w,
                         w // 4 * (h // 4) * 16, 0, 1)
    pixel_format = struct.pack("<II4s20x", 32, 0x4, b"DX10")
    caps = struct.pack("<IIII4x", 0x1000, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 98, 3, 0, 1, 0)   # BC7_UNORM, a 2D texture, one of it
    out = header + pixel_format + caps + dx10
    assert len(out) == 148
    return out + data


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    icon().save(out / "icon0.png", optimize=True)
    picture = background()
    data = bc7_encode(picture)
    decoded = bc7_decode(data, 3840, 2160)
    error = np.abs(decoded.astype(np.int32) - picture.astype(np.int32))
    assert error.max() <= 12 and error.mean() < 1.0, f"BC7: error up to {error.max()}, {error.mean():.2f} on average"
    for name in ("pic0.dds", "pic1.dds"):
        (out / name).write_bytes(dds(data, 3840, 2160))
    if len(sys.argv) > 2:
        preview = Path(sys.argv[2])
        preview.mkdir(parents=True, exist_ok=True)
        Image.fromarray(decoded).resize((1920, 1080), Image.LANCZOS).save(preview / "background.png")
    print(f"make-art: {out}/icon0.png, pic0.dds, pic1.dds (BC7 error up to {error.max()}, {error.mean():.2f} on average)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
