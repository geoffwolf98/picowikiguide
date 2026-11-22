# PicoWikiGuide

Offline Wikipedia (and other Kiwix ZIM wikis) for the ClockworkPi PicoCalc
with a Raspberry Pi Pico 2 W. A fork of WikiReader Gen2. See `DESIGN.md`
for how it works and the card format.

Version 0.8.0 (22 Nov 2026): colour pictures (zimprep --colour).

* `zimprep`: turns a ZIM into a PicoWikiGuide wiki folder.
* `pwg-sdl`: the app in a 320 x 320 window, driven by your keyboard.
* `pwg-headless`: the app driven by a script (tests).
* `pagecat`: checks a prepared wiki and draws single pages.

* `picowikiguide-VERSION.uf2`: the firmware for a PicoCalc with a
  Raspberry Pi Pico 2 or 2 W. It runs the same app code.
* `pwg_hwtest-VERSION.uf2`: a hardware test (screen, keyboard, SD card).

## On the PicoCalc

1. Make the card's files with zimprep (below), or use the release's
   `PicoWikiGuide-VERSION-card.zip`, and copy them to the top folder of
   a FAT32 SD card: each wiki folder (for example `TOP100`) and
   `PWGSTATE.DAT`. The card must be FAT32 (not exFAT), with an MBR
   partition table; 32 KB clusters are best:
   `sudo mkfs.vfat -F 32 -s 64 -n PICO /dev/sdX1` (check the device
   name with `lsblk` first: this erases it).
2. Install the firmware, one of two ways:
   * **With pelrun's UF2 Loader** (a menu of apps on the SD card): copy
     `picowikiguide-VERSION.uf2` into the card's `pico2-apps` folder and
     choose it from the loader's menu. Other apps (WebMite and so on)
     can sit beside it, so switching is a menu choice.
   * **Directly:** hold BOOTSEL on the Pico and plug it into the PC by
     USB, then copy `picowikiguide-VERSION.uf2` to the `RP2350` drive
     that appears. This replaces whatever was on the Pico (including a
     loader).
3. It shows DON'T PANIC, then opens. The keys are as in the table
   below; F1 to F5 are the top row. Hold a letter key while it starts
   to skip reopening the last page (the UF2 Loader uses some other
   held keys, such as F1, F5 and Up, to open its own menu).

After 5 minutes without a key press it switches the PicoCalc off (the
page you were on is saved after one minute, so it reopens there, and
the top bar says "Saved position"). For the last 10 seconds the top bar
counts down to the switch-off; any key stops it. With
an older keyboard firmware that cannot switch off, the screen and its
backlight go off instead and any key brings them back.

The app only overwrites `PWGSTATE.DAT`, `NAME.HIS` and `NAME.FAV` in
place. It never creates files and never changes the FAT, so it cannot
damage the card's file system.

## Building (Linux)

You need gcc and make, and for the window version the SDL2 development
package. Everything else (zstd, xz) is in `third_party/`.

```
sudo apt install libsdl2-dev        # once (Mint, Ubuntu, Debian)
mkdir -p PicoWikiGuide
tar xzf PicoWikiGuide-0.8.0.tar.gz -C PicoWikiGuide --strip-components=1
cd PicoWikiGuide
make clean                          # always, after unpacking a new version
make -j
build/zimprep --version             # check it is the new version
```

`make clean` matters when you unpack over an older copy: tar keeps the
files' original dates, so without it `make` can keep stale object files
from the old version and build a broken mix (zimprep then spins
forever).

This makes `build/zimprep`, `build/pagecat`, `build/pwg-headless`,
`build/pwg-imgtest` and,
when SDL2 is installed, `build/pwg-sdl`. If `pwg-sdl` is missing, check
that `pkg-config --libs sdl2` prints something.

### Building the firmware

You need CMake, the Arm compiler (`arm-none-eabi-gcc`) and the Raspberry
Pi Pico SDK 2.x (its submodules are not needed):

```
sudo apt install cmake gcc-arm-none-eabi
git clone -b 2.2.0 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
export PICO_SDK_PATH=~/pico-sdk
cmake -S pico -B build-pico -DPICO_BOARD=pico2
make -C build-pico -j
```

This makes `build-pico/picowikiguide.uf2` and `build-pico/pwg_hwtest.uf2`.
The first run downloads and builds the SDK's `picotool` (it needs git).

## Making a card

A "card" is a folder laid out like the SD card: one folder per wiki, plus
`PWGSTATE.DAT`. zimprep makes both:

```
build/zimprep -n TOP100 --pictures all --colour wikipedia_en_100_2026-08.zim ~/card
build/zimprep -n CLIMATE wikipedia_en_climate_change_mini_2024-06.zim ~/card
```

* `-n NAME`: the folder and file name, up to 8 letters or digits. Without
  it, the name comes from the ZIM's file name.
* `--threads N`: worker threads (default: all CPUs).
* `--level N`: zstd level (default 19).
* `--dict-kb N`: dictionary size in KiB (default and most 112: the
  PicoCalc has a fixed 112 KiB dictionary buffer).
* `--font FILE`: the Unicode fallback font (default `fonts/WIKIFONT.BIN`
  of this source). It must be the one built into the firmware.
* `--pictures none|lead|all`: pictures (default none). `lead`: the first
  picture of each article; `all`: every picture (up to 32 an article).
  They are converted to the screen's four greys (dithered), at most
  311 x 255 pixels, and kept in `NAME.I00`, `NAME.I01`, ... A picture
  goes where it is in the article if it fits on the page with its
  caption, otherwise at the top of the next page, and the page before
  says "▼ Picture on the next page ▼". The ZIM must have pictures:
  Wikipedia's `maxi` ZIMs do, `nopic` and `mini` do not (TOP100 does). A
  picture costs about 6 KB on the card; English Wikipedia with `lead`
  might need some 30 GB more (64 GB card), with `all` much more.
  Older firmware shows a blank space where a picture is.
* `--colour` (with `--pictures`): colour pictures, kept as small JPEGs
  (about 10 KB each, 1.6 times the grey ones) and shown in 256 colours
  (RGB332 with ordered dithering). `--jpeg-quality N` (default 75) trades
  size for quality; a picture is made smaller until it fits the
  PicoCalc's buffer. Firmware before 0.8.0 shows an outline instead.

Each run writes `~/card/NAME/` (`NAME.TOC`, `NAME.P00`..., `NAME.IDX`,
`NAME.HIS`, `NAME.FAV`, and with pictures `NAME.I00`...) and, if it is not there yet, `~/card/PWGSTATE.DAT`.
On the PicoCalc, the contents of `~/card` go in the top folder of a FAT32
SD card.

`PWGSTATE.DAT` must be on the card: the app never creates files, so
without it nothing is remembered between runs. zimprep makes it (once;
an existing one is kept). Copy only the wiki folder when you update a
wiki if you want the PicoCalc to keep its own history and state.

## Running the app on the PC

```
build/pwg-sdl ~/card
```

* `--scale N`: window size (default 2, so 640 x 640).
* `--safe`: start without reopening the last page (on the PicoCalc: hold
  a key while it starts).
* F12 saves a screenshot as `pwg-shot-N.png` in the current folder.

### Keys

| Key | Action |
|---|---|
| F1 | Search (press it again to clear the search) |
| F2 | History and Favourites (Tab switches, Del removes an entry) |
| F3 | Random article |
| F4 | Calculator |
| F5 | Help, wiki details; W there chooses another wiki |
| Up / Down (or Space) | Previous / next page |
| Shift+Up / Shift+Down (Page Up / Page Down on a PC) | Previous / next section |
| Left / Right | Previous / next link on the page |
| Enter | Follow the focused link |
| Esc or Backspace | Back |
| `[` `]`, or Shift+Left / Shift+Right on a PC | Previous / next section |
| L | Section list |
| F or / | Find in the article: type, Enter or Down for the next match, Up for the previous, Esc to close |
| * or 8 | Favourite on or off (a star shows in the top bar) |
| Tab / Del (or Shift+Tab / Shift+Del, Home / End on a PC) | First / last page |

Search: type a title, Up/Down to choose, Enter to open. Esc clears what
you typed, then goes back to the article.

Editing (search, find and calculator lines): Left/Right move the cursor,
typing inserts at the cursor, Backspace deletes before it and Del at it
(Del at the end clears the line). Shift+Tab / Shift+Del (Home / End)
jump to the start / end. A `>` marks a redirect (another
name for an article).

Calculator: type the sum, for example `2*sqrt(16)+sin(30)`, then Enter.
`+ - * / ^ % !`, brackets, `E` for times ten to the, `sin cos tan asin acos
atan sqrt ln log pi e ans`. Tab switches degrees and radians, Del clears.

## Checking a wiki

```
build/pagecat ~/card/TOP100/TOP100 --check
build/pagecat ~/card/TOP100/TOP100 --sections Evolution
build/pagecat ~/card/TOP100/TOP100 Evolution 2 -o page.png
```

The last form draws page 2 of an article. An optional number after the
page is an item to show as the focused link.

## Tests

```
CARD=~/testcard sh tests/run.sh
```

`~/testcard` must hold CLIMATE (from
`wikipedia_en_climate_change_mini_2024-06.zim` in openzim's
zim-testing-suite) and TOP100 (`wikipedia_en_100_2026-08.zim`), made
with the commands above. The tests reset that card's history,
favourites and state.

`pwg-imgtest` runs the same scripts on a FAT32 card image with the
PicoCalc's own file code: `build/pwg-imgtest card.img tests/scripts/basic.txt`.
`tests/run.sh` makes that image itself (`tests/mkimage.py`) when
pyfatfs is installed (`pip install pyfatfs`).

The scripts run with `PWG_HEAP=118212`: a heap like the PicoCalc's (the
firmware's free RAM: 0x20080000 minus the `end` symbol, from
`arm-none-eabi-nm build-pico/picowikiguide.elf`), allocated the way
newlib does, so running out of memory, holes and leaks show up on the
PC. `tests/scripts/memory.txt` checks that the free memory comes back
exactly and stays in one block. `pagecat --index-check` searches for
every title and prefixes of it with several index summary steps.

## Changes

* 0.5.6: the focused link has a box round it and a solid underline
  instead of being shown inverted (white on black blurred the smooth
  text); a find match has a light grey background. Wikis from 0.5.x
  still work.
* 0.5.7: favourites and history remember the page you were on when you
  leave an article, so they reopen where you left off.
* 0.5.8: choosing another wiki restarts the PicoCalc straight into it
  (no splash), so it opens with all the memory free, as at power-on;
  this is meant to stop a crash going from TOP100 to ENPEDIA. Switched off on the
  search screen, it starts on the search screen with the search as it
  was. Out of memory now gives a message instead of stopping.
* 0.5.9: "Saved position" shows in the top bar for 10 seconds after the
  idle save (a minute without keys); for the last 10 seconds before the
  idle switch-off (5 minutes) the top bar counts down, and any key stops
  it (that key does nothing else). The help screen (F5) shows the free
  memory and the largest free block.
* 0.6.0: the Unicode fallback font is compressed (1.95 MB to 0.71 MB, in
  blocks of 96 glyphs, inflated when a character needs one), so the
  firmware is 1.3 MB and the UF2 file 2.6 MB instead of 5 MB. Every
  character is still there and draws the same; wikis do not need
  preparing again (the font fingerprint is unchanged). About 18 KB more
  memory is free (the splash and messages share the app's frame buffer).
* 0.6.1: memory: the page, dictionary and decompression buffers are
  fixed (no longer taken from the heap each time a wiki opens), the
  title search keeps every 512th index page in memory instead of every
  128th (about 16 KB less with English Wikipedia, a few ms more per
  search), and opening files no longer takes and frees 16 KB blocks; so
  the free memory stays in one piece. The selected row in lists and the
  History / Favourites tab get a box and underline like a focused link
  instead of being inverted. In an article Tab and Del go to the first
  and last page, 8 sets a favourite like *, and F finds like /.
* 0.6.2: the wiki's compression dictionary (112 KB) is kept in the
  Pico's flash instead of RAM: about 112 KB more free memory. It is
  copied to the last 112 KB of the flash when you open a wiki that is
  not the one copied last (a second or so, "Setting up this wiki"); the
  same wiki again writes nothing. With the UF2 Loader this is the end of
  its app partition, the same place.
* 0.6.3: the help screen mentions Space (next page) and shows how the
  free memory lies: in holes between blocks in use, or at the top of the
  heap in one piece.
* 0.7.0: pictures. `zimprep --pictures lead` or `all` converts the ZIM's
  pictures to the screen's four greys (Floyd-Steinberg dithering, at most
  311 x 255) into `NAME.I00`...; the PicoCalc draws them in the page. A
  picture that does not fit on a page goes to the next one with its
  caption, and the page before says so. Wikis without pictures are made
  exactly as before. zimprep has libwebp's decoder built in (no extra
  packages).
* 0.7.1: zimprep: a heading followed by a picture too tall to share its
  page no longer ends up split over pages of their own (the whole
  heading stays, then "Picture on the next page"); `pagecat --check`
  reports pages with only a heading. Remake picture wikis.
* 0.7.2: zimprep: a heading may end a page when a picture follows (it
  has to go to the next page anyway), so a page is no longer left mostly
  empty for it; the note also shows in smaller gaps. TOP100P: 264 pages
  fewer, pages less than half full 823 to 479 (of 14,848). `pagecat
  --check` counts those. Remake picture wikis.
* 0.7.3: zimprep could hang (rarely, at random) in the HTML parser: two
  threads built its tag table at once and could link an entry to itself.
  The tables are now built before the threads start. Wikis it finished
  are fine. The card zip and the tests have one TOP100, with pictures
  (no TOP100P).
* 0.8.0: colour pictures. `zimprep --pictures all --colour` keeps each
  picture as a small JPEG; the PicoCalc decodes it (TJpgDec) into a
  fixed 79 KB picture buffer as RGB332 with ordered dithering and shows
  it over the grey screen: the LCD driver takes those pixels from the
  picture as it sends the rows, and resends a picture's rows whenever
  the pictures change, so nothing is left half painted (tested against
  a simulated panel). The buffer holds all of a page's pictures, grey or
  colour; it replaces the 20 KB grey one, so about 60 KB less is free.
  The hardware test shows red, green and blue bars to check the panel's
  colour order. TOP100 in the card zip is in colour.

* 0.5.5: README: installing through pelrun's UF2 Loader (`pico2-apps`),
  `make clean` after unpacking a new version, FAT32 card format, the
  release's card zip. `.gitignore` leaves out `.uf2` files. No change to
  the app or the card format.

* 0.5.4: F1 from an article goes back to the search list where you left
  it (scrolled and selected), not to the top of the results. F1 on the
  search screen still clears it.

* 0.5.3: a start-up screen (DON'T PANIC in large friendly letters, made
  by `tools/mksplash.py`), shown as soon as the screen is ready. The top
  bar says "Press F5 for help" for the first 10 seconds, or until a key
  is pressed.

* 0.5.2: switches itself off after 5 minutes without a key (through the
  keyboard controller's power-off register; with older keyboard firmware
  the screen goes dark until a key is pressed).

* 0.5.1: the search, find and calculator lines can be edited: a cursor
  that Left/Right move, insert at the cursor, Backspace and Del either
  side of it, Home/End. Wikis from 0.5.0 still work (no rebuild).

* 0.5.0: the Unicode fallback font from WikiReader Gen2 (`fonts/WIKIFONT.BIN`,
  1.95 MB, Unifont and WenQuanYi, the whole BMP) is built into the firmware's
  flash and used by zimprep and the PC builds: Chinese, Japanese, Korean,
  Cyrillic, Hebrew, Arabic, Indic and other scripts show instead of boxes
  (1 bit, Arabic not joined, Indic not shaped). No extra RAM. The .uf2 is
  about 5 MB. Wikis must be prepared again. `tools/mkwikifont.py` (from
  Gen2) rebuilds the font.

* 0.4.1: the small font (top bar, hints, help, small print in articles)
  is back to 1 bit, which is crisper than greys at 12 px, above all as
  white on black. Wikis must be prepared again.

* 0.4.0: smooth text: the 18 px fonts have 4 grey levels (made by
  `tools/ttf2grey.py` from Liberation) and the screen is drawn with 2 bits
  a pixel. Wikis must be prepared again with this zimprep (the app says so
  for older ones). Shift+Up / Shift+Down move by section (the PicoCalc
  keyboard sends nothing for Shift+Left / Shift+Right). F1 on the search
  screen clears the search. pagecat and screenshots write grey PNGs (or
  PGM).

* 0.3.2: first PicoCalc firmware: screen, keyboard (keys repeat when
  held), SD card, read-only FAT32 with in-place saving of state,
  history and favourites. `pwg-imgtest` tests that file code on a card
  image.
* 0.3.1: hardware test: screen updates fixed (the window command is
  sent under one chip select); status line with keyboard details.
* 0.3.0: hardware test for the PicoCalc.

* 0.2.0: the app: wiki list, search, pages, link focus and following,
  Back, section list and section jumps, find in article, history,
  favourites, random article, calculator, help. Window and scripted PC
  versions. zimprep also writes `PWGSTATE.DAT`. pagecat writes PNG,
  rejects unknown options and says when a title is not found.
* 0.1.0: first version. zimprep (layout on the PC, 18 px pages, zstd with a
  dictionary per wiki, sections, links resolved to article numbers, Gen2's
  title index), the page reader and drawing code, and pagecat.
