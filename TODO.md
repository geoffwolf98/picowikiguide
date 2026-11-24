# PicoWikiGuide: to do

* 1.0.0 (zimprep): make `--pictures all --colour` the default (`--grey`
  and `--pictures none` to opt out; say "no pictures in this ZIM" for
  nopic/mini ZIMs); print the version and the picture settings at the
  start of every run; rebuild when PWG_VERSION changes (make kept an old
  zimprep after unpacking a release); put the measured ENPEDIA size in
  the README table.
* 1.0.0 (zimprep): section redirects. Kiwix ZIMs store a redirect to a
  section (`Croaking` to `Frog#Call`, "50 United States") as a tiny
  HTML page with `<meta http-equiv="refresh" content="0;URL='./T#S'">`,
  and zimprep makes each a one-line article (TOP100: 1221 of its 1323
  "articles"). Find them (under about 2 KB, an instant refresh, the
  target found in the ZIM, followed through redirects to a real
  article), make them search entries marked as redirects, with the
  section's page and title when `#S` matches a heading (the app puts
  the heading at the top, as from 0.9.7); otherwise keep the page.
  Read the HTML clusters in stored order to check the small pages
  (title order would fetch them at random). Print the counts.
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
