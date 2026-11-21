#!/usr/bin/env python3
"""Render glyphs of a TrueType font with 4 grey levels into a BDF-like
file for bdf2c.py (PicoWikiGuide's smooth text).

  ttf2grey.py FONT.ttf PIXELS ASCENT DESCENT CODEPOINTS OUT.bdf

CODEPOINTS: a file of hex code points or ranges (03b1-03c9), or an
existing BDF whose ENCODING lines give the set.  The output has the
property BITS_PER_PIXEL 2; each bitmap row is ceil(width / 4) bytes,
2 bits a pixel, MSB first, 0 = no ink .. 3 = full ink.  Uses FreeType's
light hinting (sharp vertically, true shapes horizontally).
Needs freetype-py (pip install freetype-py).
"""
import sys
import freetype


def codepoints(path):
    text = open(path, encoding="latin-1").read()
    if text.startswith("STARTFONT"):
        # a BDF: its ENCODING lines give the set
        out = [int(ln.split()[1]) for ln in text.splitlines() if ln.startswith("ENCODING ")]
    else:
        out = []
        for ln in text.splitlines():
            for tok in ln.split("#")[0].split():
                if "-" in tok:
                    a, b = tok.split("-")
                    out += range(int(a, 16), int(b, 16) + 1)
                else:
                    out.append(int(tok, 16))
    return sorted(set(c for c in out if 0 <= c <= 0xFFFF))


def main():
    ttf, px, asc, desc, cps, outp = sys.argv[1:7]
    px, asc, desc = int(px), int(asc), int(desc)
    face = freetype.Face(ttf)
    face.set_pixel_sizes(0, px)
    chars = []
    for cp in codepoints(cps):
        if not face.get_char_index(cp):
            continue
        face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        nb = (w * 2 + 7) // 8
        rows = []
        for r in range(h):
            line = bytearray(nb)
            for x in range(w):
                a = bm.buffer[r * bm.pitch + x]
                v = (a * 3 + 127) // 255		# 0..3
                line[x >> 2] |= v << (6 - 2 * (x & 3))
            rows.append(bytes(line).hex().upper())
        adv = (g.advance.x + 32) >> 6
        chars.append((cp, adv, w, h, g.bitmap_left, g.bitmap_top - h, rows))
    with open(outp, "w") as f:
        f.write("STARTFONT 2.1\nFONT -pwg-%s-%d-grey\nSIZE %d 75 75\n" % (face.family_name.decode(), px, px))
        f.write("FONTBOUNDINGBOX %d %d 0 %d\n" % (px * 2, asc + desc, -desc))
        f.write("STARTPROPERTIES 3\nFONT_ASCENT %d\nFONT_DESCENT %d\nBITS_PER_PIXEL 2\nENDPROPERTIES\n"
                % (asc, desc))
        f.write("CHARS %d\n" % len(chars))
        for cp, adv, w, h, xo, yo, rows in chars:
            f.write("STARTCHAR U+%04X\nENCODING %d\nSWIDTH 500 0\nDWIDTH %d 0\nBBX %d %d %d %d\nBITMAP\n"
                    % (cp, cp, adv, w, h, xo, yo))
            for r in rows:
                f.write(r + "\n")
            f.write("ENDCHAR\n")
        f.write("ENDFONT\n")
    print("%s: %d glyphs" % (outp, len(chars)))


main()
