#!/usr/bin/env python3
"""Make src/splash.c: the start-up words in large letters, 2 bits a pixel
(0 = no ink .. 3 = full), drawn by the firmware on a black screen.

  mksplash.py FONT.ttf OUT.c
Needs freetype-py.
"""
import sys
import freetype

LINES = ["DON'T", "PANIC"]
MAX_W, MAX_H, GAP = 300, 250, 10


def render(face, size):
    face.set_pixel_sizes(0, size)
    out = []
    for text in LINES:
        glyphs, x = [], 0
        for ch in text:
            face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
            g = face.glyph
            b = g.bitmap
            glyphs.append((x + g.bitmap_left, g.bitmap_top, b.width, b.rows, bytes(b.buffer), b.pitch))
            x += g.advance.x >> 6
        out.append((x, glyphs))
    return out


def main():
    face = freetype.Face(sys.argv[1])
    size = 140
    while True:
        lines = render(face, size)
        asc = face.size.ascender >> 6
        cap = max(top for _, gl in lines for (_, top, _, _, _, _) in gl)
        w = max(x for x, _ in lines)
        h = cap * len(LINES) + GAP * (len(LINES) - 1)
        if w <= MAX_W and h <= MAX_H:
            break
        size -= 2
    img = [[0] * w for _ in range(h)]
    for i, (lw, gl) in enumerate(lines):
        x0 = (w - lw) // 2
        base = cap + i * (cap + GAP)
        for gx, top, gw, gh, buf, pitch in gl:
            for r in range(gh):
                for c in range(gw):
                    a = buf[r * pitch + c]
                    y, x = base - top + r, x0 + gx + c
                    if 0 <= y < h and 0 <= x < w:
                        img[y][x] = max(img[y][x], (a * 3 + 127) // 255)
    nb = (w * 2 + 7) // 8
    data = bytearray()
    for row in img:
        line = bytearray(nb)
        for x, v in enumerate(row):
            line[x >> 2] |= v << (6 - 2 * (x & 3))
        data += line
    with open(sys.argv[2], "w") as f:
        f.write("/* made by tools/mksplash.py: do not edit */\n#include \"splash.h\"\n\n")
        f.write("const int splash_w = %d, splash_h = %d;\n" % (w, h))
        f.write("const uint8_t splash_bits[%d] = {\n" % len(data))
        for i in range(0, len(data), 16):
            f.write("\t" + ", ".join("%d" % b for b in data[i:i + 16]) + ",\n")
        f.write("};\n")
    print("%dpx letters, %d x %d, %d bytes" % (size, w, h, len(data)))


main()
