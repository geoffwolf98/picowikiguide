#!/usr/bin/env python3
"""Compress the Unicode fallback font: ZWFT (from bdf2c.py --bin) to ZWF2.

  packwikifont.py WIKIFONT-ZWFT.BIN WIKIFONT.BIN
  PWG_WIKIFONT=WIKIFONT.BIN build/pagecat --font-fingerprint --patch

The glyphs are cut into blocks of 96; each block holds the glyphs'
metrics (5 bytes each: advance, x offset, top, width, height) and then
their bitmaps, compressed with raw deflate.  The device inflates one
block (at most 96 * (5 + 32) bytes) when a character needs it.

ZWF2 layout (little endian):
  0  "ZWF2"          4  u32 glyphs        8  u8 ascent   9  u8 descent
  10 u8 per block    11 u8 ranges         12 u32 blocks
  16 u32 fingerprint state before this font (0: not filled in)
  20 u32 fingerprint with this font (the same value the ZWFT form gives)
  24 u32 bitmap bytes in all               28 u32 0
  32 ranges: u16 first code point, u16 count, u32 first glyph
  then (blocks + 1) u32 offsets of the blocks from the data start
  then the data.
The two fingerprint fields are filled in by pagecat --font-fingerprint
--patch, which works them out the slow way (inflating everything).
"""
import struct
import sys
import zlib

PER_BLOCK = 96


def main():
    src, dst = sys.argv[1:3]
    d = open(src, "rb").read()
    if d[:4] != b"ZWFT":
        sys.exit("%s: not a ZWFT font" % src)
    n, asc, desc, _ = struct.unpack_from("<IBBH", d, 4)
    recs = [struct.unpack_from("<HBbbBBBI", d, 12 + 12 * i) for i in range(n)]
    bits = d[12 + 12 * n:]
    offs = [r[7] for r in recs] + [len(bits)]
    for i, r in enumerate(recs):
        if offs[i + 1] - offs[i] != ((r[4] + 7) // 8) * r[5] or r[4] > 16 or r[5] > 16:
            sys.exit("glyph %04x: unexpected bitmap size" % r[0])
    ranges = []
    for i, r in enumerate(recs):
        if ranges and r[0] == ranges[-1][0] + ranges[-1][1] and ranges[-1][1] < 65535:
            ranges[-1][1] += 1
        else:
            ranges.append([r[0], 1, i])
    if len(ranges) > 255:
        sys.exit("too many code point ranges")
    blocks = []
    for s in range(0, n, PER_BLOCK):
        e = min(n, s + PER_BLOCK)
        raw = b"".join(struct.pack("<BbbBB", *recs[i][1:6]) for i in range(s, e))
        raw += bits[offs[s]:offs[e]]
        assert len(raw) <= 4096
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
        blocks.append(c.compress(raw) + c.flush())
    out = bytearray(b"ZWF2")
    out += struct.pack("<IBBBBIIIII", n, asc, desc, PER_BLOCK, len(ranges), len(blocks),
                       0, 0, len(bits), 0)
    for first, cnt, gi in ranges:
        out += struct.pack("<HHI", first, cnt, gi)
    pos = 0
    for b in blocks:
        out += struct.pack("<I", pos)
        pos += len(b)
    out += struct.pack("<I", pos)
    for b in blocks:
        out += b
    open(dst, "wb").write(out)
    print("%s: %d glyphs, %d ranges, %d blocks, %d bytes (from %d)" %
          (dst, n, len(ranges), len(blocks), len(out), len(d)))


if __name__ == "__main__":
    main()
