# PicoWikiGuide: to do

* Battery level in the top bar. The keyboard controller already gives it
  (register 0x0b, `kbd_battery()` in `pico/kbd.c`, shown raw by the
  hardware test); needs the scale checked on the device, then a small
  icon or percentage, refreshed with the 1 s tick.
* Pictures: an option to let text fill the rest of a page and put a
  picture that does not fit at the top of the next (no near-empty page
  under a title).
* Pictures: check the greys and the drawing speed on the device; tune
  the dithering if photos look too dark or too light.
* A `maxi` ZIM build of English Wikipedia with `--pictures lead`.
* Memory: work out the 64 KB gap between free memory and the largest
  block on the device (F5 now shows holes and top).
