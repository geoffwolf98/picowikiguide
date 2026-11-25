# PicoWikiGuide: to do

* Section redirects (1.0.0) find their heading for 75% of TOP100's
  (839 of 1114); the rest open at the top: anchors inside another
  heading (Amazon's "Website" in "Amazon.com"), in tables, or removed
  sections. zimprep could note each anchor's page while laying out the
  target and store the page instead.
* Slideshow (1.1.0): tune on ENPEDIA (TOP100 has one list): the
  first-on-line link filter, random spread on long lists, speed of a
  step (at most 200 articles tried, lead picture from the first 3
  pages).
* Next: slideshow option "Switch off at low battery: 20% / never"
  (default 20%): while a show runs and not charging (bit 7 clear), at
  20% or less: "Battery low: switching off", save the place, power off
  (as Shift+O). On charge it never triggers, so a show can run for ever.
  Kept in a spare state byte (116 on; 43..47 are full). Test with
  PWG_BATTERY.
* `pagecat --article N` (by number), to look at the 23 pages with only a
  heading in the ENPEDIA maxi build (e.g. articles 307851, 2608629,
  2608630, 2812148, 2937398).
