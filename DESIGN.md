# PicoWikiGuide: design

Offline Wikipedia (and other Kiwix ZIM wikis) on the ClockworkPi PicoCalc
with a Raspberry Pi Pico 2 W. A fork of WikiReader Gen2 (zimwiki): the
title search, FAT code, calculator and fonts come from Gen2, everything
else is new.

The main idea: **all the hard work is done on the PC.** `zimprep` turns
each article into ready laid-out pages for the PicoCalc screen. The device
only finds a page, decompresses it (a few hundred bytes) and draws it.

## 1. Project rules

* Anonymous: no names or email addresses in the source, docs or commits.
* `PWG_VERSION` in `src/zw.h` is bumped on every build handed out, with a
  changelog line in `README.md`.
* Step-by-step build instructions with every release.
* No em dashes in docs or messages.
* Version 1 is deliberately basic. The card format leaves room to add
  features later (section 4.4).

## 2. Target

| | |
|---|---|
| Board | Raspberry Pi Pico 2 W (RP2350, 520 KB SRAM, 4 MB flash) in a PicoCalc |
| Screen | 320 x 320, drawn with 2 bits a pixel (4 greys), sent to the LCD as 18 bit colour |
| Input | PicoCalc keyboard (STM32 over I2C), no touch |
| Storage | SD card, FAT32, read by sector |
| Wi-Fi | not used (its driver is not linked) |

RAM budget on the device: zstd decoder context (about 100 KB) plus the
wiki's dictionary (112 KB) plus one page (under 2 KB) plus the frame buffer
(25.6 KB, and the LCD driver's copy) plus search and list buffers. Well under 520 KB. Fonts live in
flash and cost no RAM.

## 3. Version 1 scope

* Text only: no pictures, no footnotes, no citations.
* Formulas as linear text (Gen2's tex.c, run on the PC).
* Infoboxes kept at the top of the article, as in Gen2.
* One text size, 18 px: Liberation with 4 grey levels (tools/ttf2grey.py).
  14 lines per page. The screen is drawn with 2 bits a pixel.
* Links to articles that are not in the wiki are plain text.
* Features: title search, reading by page, link focus, section list,
  find in article, back, history, favourites, calculator, several wikis
  per card.

### Keys

| Key | Action |
|---|---|
| F1 | Search; again on the search screen: clear it |
| F2 | History / Favourites |
| F3 | Random article |
| F4 | Calculator |
| F5 | Help (key list) and options |
| Up / Down | Scroll a line (at most a third of a screen) |
| G | Go to page (number bar) |
| Space / B, Shift + Space | A screen on / back |
| Left / Right | Previous / next link on the page |
| Enter | Follow the focused link |
| Esc or Backspace | Back |
| Shift + Down / Up (the keyboard sends Page Down / Page Up) | A screen on / back |
| N / P | Next / previous section |
| L | Section list |
| / | Find in article |
| * | Favourite on / off |
| Shift + Tab / Shift + Del (Home / End) | First / last page |

In lists (search results, history, sections): Up/Down select, Enter opens,
Esc goes back. In Find: type, Enter or Down for the next match, Up for the
previous one.

## 4. Card format

Each wiki is a folder with an 8.3 name, for example `ENPEDIA/`:

```
ENPEDIA/ENPEDIA.TOC    header, dictionary and tables
ENPEDIA/ENPEDIA.P00..  page data, in pieces of up to 2 GiB
ENPEDIA/ENPEDIA.IDX    title index (Gen2 format; target = article number)
ENPEDIA/ENPEDIA.HIS    history placeholder (64 KB of zeros)
ENPEDIA/ENPEDIA.FAV    favourites placeholder (32 KB of zeros)
/PWGSTATE.DAT          state (last wiki, last page, options)
```

As in Gen2 the device never creates or grows a file: zimprep (or the card
README) makes the placeholders and the device overwrites them in place.

All numbers are little-endian.

### 4.1 NAME.TOC

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `PWGT` |
| 4 | 2 | format version (1) |
| 6 | 2 | header size (4096) |
| 8 | 4 | must-understand flags (none in version 1; the device refuses a wiki with a bit it does not know) |
| 12 | 4 | content flags (bit 0 footnotes, 1 citations, 2 formula bitmaps, 3 pictures; all 0 in version 1; informational) |
| 16 | 1 | compression: 1 = zstd with the dictionary below |
| 17 | 1 | pages per frame (1 in version 1) |
| 18 | 1 | text size in px (18) |
| 19 | 1 | piece shift (31 = 2 GiB pieces) |
| 20 | 4 | font fingerprint (FNV-1a of the font tables used) |
| 24 | 2 | page width (320) |
| 26 | 2 | page height (body area, below the top bar) |
| 28 | 4 | articles |
| 32 | 4 | frames |
| 36 | 4 | pieces |
| 40 | 4 | main page article (0xFFFFFFFF = none) |
| 44 | 4 | dictionary offset in TOC |
| 48 | 4 | dictionary length |
| 52 | 4 | article table offset |
| 56 | 4 | frame table offset |
| 60 | 4 | section table offset |
| 64 | 8 | section table length |
| 72 | 8 | page data length (all pieces) |
| 80 | 16 | zimprep version |
| 96 | 128 | title (from the ZIM) |
| 224 | 16 | language |
| 240 | 16 | name (8.3 folder name) |
| 256 | 16 | date of the ZIM |
| 272 | 16 | source ZIM UUID |

**Article table** (12 bytes per article): u32 first frame, u16 pages,
u16 sections, u32 offset of the article's sections in the section table
(0xFFFFFFFF = none).

**Frame table** (8 bytes per frame): u40 offset in the page data, u24
compressed length. Piece = offset >> piece shift. A frame never crosses
a piece boundary.

**Section table**, per section: u16 page, u8 level (2 to 4), u8 title
length, title (UTF-8).

### 4.2 Frames and pages

A frame is one zstd frame (with the wiki's dictionary). Decompressed:

```
u8 npages, then npages x u16 page length, then the pages
```

A page:

```
u8 nlines
per line:
  u16 y (from the top of the body area), u8 height, u8 baseline,
  u8 deco (0 none, 1 bullet, 2 number, 3 rule, 4 heading rule; bit 7 = quote bar)
  if deco 1 or 2: i16 deco x, u16 number
  u8 nitems
  per item: u8 type
    type < 0x80: text in font `type`:
        i16 x, i8 dy (sup/sub shift), u8 link slot (0xFF = none),
        varint byte length, UTF-8 bytes
    type >= 0x80: varint length, payload (skipped by version 1 firmware)
u8 nlinks, then nlinks x varint target article number
```

The link rectangles are worked out on the device from the item's x,
the line's y and height, and the text width (same font tables as zimprep).

### 4.3 Fonts

Characters the 18 px fonts lack come from `fonts/WIKIFONT.BIN` (Gen2's
Unicode fallback, 1 bit, 57,313 glyphs), which the firmware keeps in
flash and zimprep loads for its widths; the fingerprint covers it.

The file is compressed (format ZWF2, see `tools/packwikifont.py`): 22
code point ranges map a character to a glyph number; glyphs are in
blocks of 96, each block their metrics (5 bytes) then their bitmaps,
raw deflate. The device inflates one block into a 4 KB buffer
(`src/inflate.c`, after puff) and copies the glyph out; 0.71 MB instead
of 1.95 MB. zimprep inflates the whole font at start (its threads cannot
share the block buffer). The fingerprint is the one the uncompressed
(ZWFT) form gives; the font stores it, with the built-in fonts' hash it
was worked out against, so the device need not inflate everything.
`pagecat --font-fingerprint` checks both, and that every character
draws the same both ways.

### 4.4 Memory on the device

The big buffers are fixed in .bss, not taken from the heap: the page
frame (16 KB) and a second one for the page below while scrolling
(16 KB), the compressed frame (16.5 KB), the zstd context (96 KB)
and dictionary tables (28 KB). The dictionary itself (112 KB at most)
is only read once loaded, so the PicoCalc keeps it in flash
(`pico/dictflash.c`, `src/dictsync.c`): the last 112 KB of the flash
the firmware sees through QMI address translation window 0 (all 4 MB
when flashed with BOOTSEL; the app partition from 8 KB under the UF2
Loader; the same physical place either way). Opening a wiki compares
the card's dictionary with the copy, through the compressed frame
buffer, and erases and writes only the 4 KB sectors that differ (with
the SDK's flash functions, interrupts off), so the same wiki writes
nothing and a copy cut short is finished next time. zstd reads it from
flash by reference. PC builds keep it in RAM. One
wiki is open at a time. While no page is being read the compressed
frame buffer is lent to the FAT code for walking cluster chains when a
file is opened. The heap then holds only small things that live as
long as the wiki (file records, extents, the index summary), so it
does not split into pieces. The title index keeps every 512th page's
top entry in memory (from the file's every-128th summary); a search
then binary-searches the top entries of one 512 page span on the card
(about 9 reads of 20 bytes, mostly from the block cache).


zimprep and the firmware are built from the same `fonts/fonts.c`. The
glyph widths zimprep lays out with are exactly the ones the device draws
with. The fingerprint in the TOC catches a wiki made with different fonts;
the device then asks for the wiki to be prepared again.

### 4.5 Pictures

`zimprep --pictures lead|all` (`prep/pictures.c`, after Gen2's
`tools/pictures.c`) scans every article for `<img>` (no maths, icons or
SVG; the first only with `lead`, up to 32 with `all`), converts each
picture once: decoded (WebP: the vendored libwebp decoder; PNG, JPEG,
GIF: stb_image), scaled to at most 311 x 255 (never enlarged; under 24
px dropped), grey, contrast stretched (1% cut), lightly sharpened, and
Floyd-Steinberg dithered in linear light to the screen's four greys
(panel codes FC, 9C, 4C, 00). Gen2's HTML parser and layout place them
(a picture block is a line of its height, centred; its caption follows).

`NAME.I00`, `NAME.I01`, ... (2 GiB pieces, read as one):

```
header (64): "PWGI", u32 version 1, u32 count, u32 converted,
             u64 total size, 0...
table (from 64): count x 8: u40 offset, u24 length (0: not converted)
record: u16 w, u16 h, u8 format, 3 x 0, then:
        format 0: the pixels, 2 bits each (0 white ... 3 black), 4 a byte,
                  first in the top bits, rows padded to bytes
        format 1: the same compressed with zstd (no dictionary)
        format 2: colour, a baseline JPEG (zimprep --colour)
        records past the header are at most 16000 bytes (they go through
        the compressed frame buffer)
```

The TOC header has the picture count at 288 (0: no pictures); the
firmware opens the I pieces only then and checks the count against the
file. In a page a picture is item type 0x81 (PI_PIC): varint length 10,
i16 x, u16 w, u16 h, u32 number, on a line of the picture's height.
Colour (format 2) pictures are decoded with TJpgDec (`third_party/tjpgd`,
RGB888 out in 8 x 8 or 16 x 16 blocks, 3.5 KB work area) into RGB332 with a
4 x 4 ordered dither. They are not drawn into the 2 bit frame buffer: each
is a rectangle listed in `src/overlay.c`, and `src/present.c` (the LCD
driver's changed-rows sender, also used by the PC builds' screenshots and
window) takes those pixels from the picture as it makes each row's RGB.
A row is sent when its frame buffer bytes changed or, when the list of
pictures changed (position, size, pixels or a generation number bumped
whenever the buffer is refilled), when a picture covers it now or did
last time. `tests/ovtest.c` checks this against a simulated panel. The
picture buffer holds all the pictures of the view last drawn, grey (2
bits a pixel) or colour (a byte a pixel); a view whose pictures are not
all there refills it from the start: whole pictures if they all fit,
else only their visible rows, the most showing first, and what still
does not fit decoded to dithered grey (a quarter of the room). It is
made from the heap at start, before any wiki: by default ("always
colour") 320 x 300, a whole screen, so nothing ever needs to go grey;
with "grey if short of room" (F5, C; kept in PWGSTATE.DAT, applied by
restarting) 311 x 255 bytes, 16 KB less. If the big one cannot be made
the normal one is.

Pagination keeps a picture with its caption; a picture that does not fit
in what is left of a page goes to the top of the next, and that page
gets a last line "▼ Picture on the next page ▼". Redrawing a page reads
nothing (its pictures stay decoded in the picture buffer).

### 4.6 Adding things later

* New page item types (formula bitmaps, pictures) use type >= 0x80, which
  older firmware skips.
* A feature older firmware must not ignore sets a must-understand bit.
* Pages per frame, compression and text size are header fields, so they
  change by running zimprep again, not by changing the format.

## 5. zimprep

```
zimprep [options] INPUT.zim OUTDIR
  -n NAME        8.3 folder name (default from the ZIM name)
  --threads N    default: all CPUs
  --level N      zstd level (default 19)
  --dict-kb N    dictionary size (default and most 112)
  --pictures M   none (default), lead or all
```

Steps:

1. Read the ZIM (Gen2's zim.c: zstd and xz clusters).
2. Number the articles: every HTML entry that is not a redirect.
3. Train the zstd dictionary on pages from a sample of articles.
4. For each article (in parallel): HTML to document (Gen2's html.c,
   footnotes and citations off), layout at 320 px with the 18 px fonts
   (Gen2's layout.c), split into pages (a heading is never left at the
   bottom of a page), resolve each link to an article number, encode
   and compress the pages.
5. Write the pieces, the TOC and the title index (Gen2's format).
6. Write the placeholders.

Pictures, video, audio, styles, scripts and the full-text index are never
copied: only the text of the articles reaches the card.

## 6. Code layout

```
src/     portable C used by the device and the PC builds
prep/    PC only: zimprep, Gen2's ZIM reader, HTML converter and layout
host/    PC builds of the app: SDL window and headless test runner
pico/    PicoCalc platform layer and pico-sdk build
tests/   tests
fonts/   BDF sources and generated tables
third_party/  zstd (decoder for the device, full library for zimprep), xz
```
