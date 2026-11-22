# PicoWikiGuide: to do

* Colour pictures: check the panel's colour order on the device (the
  hardware test shows red, green and blue bars top down); if red and blue
  are swapped, swap them in `src/overlay.c` (ov_row_rgb).

* Battery level in the top bar. The keyboard controller already gives it
  (register 0x0b, `kbd_battery()` in `pico/kbd.c`, shown raw by the
  hardware test); needs the scale checked on the device, then a small
  icon or percentage, refreshed with the 1 s tick.
* Pictures: an option to let text fill the rest of a page and put a
  picture that does not fit at the top of the next (no near-empty page
  under a title).
* Pictures: check the greys and the drawing speed on the device; tune
  the dithering if photos look too dark or too light.
* `pagecat --article N` (by number), to look at the 23 pages with only a
  heading in the ENPEDIA maxi build (e.g. articles 307851, 2608629,
  2608630, 2812148, 2937398).
* Pictures, option B: let text flow past a picture that does not fit, to
  cut the 2.9% of pages less than half full (ENPEDIA maxi: 2.84 million
  of 99.4 million).
* Memory: work out the 64 KB gap between free memory and the largest
  block on the device (F5 now shows holes and top).
