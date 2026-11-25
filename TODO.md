# PicoWikiGuide: to do

* Section redirects (1.0.0) find their heading for 75% of TOP100's
  (839 of 1114); the rest open at the top: anchors inside another
  heading (Amazon's "Website" in "Amazon.com"), in tables, or removed
  sections. zimprep could note each anchor's page while laying out the
  target and store the page instead.
* After 1.0.0: picture slideshow (F5 then S, listed: its own search
  box, filled with the open article's title, or type a theme such as
  "list of computers"; the normal title search; Enter starts from the
  chosen article; the options one key away. The F5 table is full: merge
  two rows, e.g. C into the W line). Themes: the
  article's own pictures, or the lead pictures of the articles a list
  links to, 1 or 2 levels deep (a linked "List of"/"Lists of" article is
  gone into, not shown). Walk lazily with a tiny stack (article, page,
  link per level); fetch the next picture while one is shown. Only
  links first on their line (list and table items; drops countries,
  years, see-also), a memory of the last ~500 articles against repeats,
  a history of ~20 for Left. Options screen, kept in the state: lead
  only / all pictures, depth 1 / 2, links per list 10 / 20 / 50 (50 the
  most, so a list's walk stays short),
  in order / random (a random start page per list and skipping links
  at a rate from the list's size), 3 / 5 / 10 / 20 s or manual. Picture
  full screen with caption and title; Enter opens the article, Esc
  back; no idle switch-off while it runs. Needs ENPEDIA (or a bigger
  test ZIM) to tune; after the section redirect fix, as lists link
  through redirects.
* Battery level in the top bar. The keyboard controller already gives it
  (register 0x0b, `kbd_battery()` in `pico/kbd.c`, shown raw by the
  hardware test); needs the scale checked on the device, then a small
  icon or percentage, refreshed with the 1 s tick.
* `pagecat --article N` (by number), to look at the 23 pages with only a
  heading in the ENPEDIA maxi build (e.g. articles 307851, 2608629,
  2608630, 2812148, 2937398).
