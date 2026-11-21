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
| Up / Down | Previous / next page |
| Left / Right | Previous / next link on the page |
| Enter | Follow the focused link |
| Esc or Backspace | Back |
| Shift + Up / Down (the keyboard sends Page Up / Page Down) | Previous / next section |
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


zimprep and the firmware are built from the same `fonts/fonts.c`. The
glyph widths zimprep lays out with are exactly the ones the device draws
with. The fingerprint in the TOC catches a wiki made with different fonts;
the device then asks for the wiki to be prepared again.

### 4.4 Adding things later

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
  --dict-kb N    dictionary size (default 112)
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
