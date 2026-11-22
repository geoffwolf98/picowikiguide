/*
 * PicoWikiGuide zimprep --pictures: the articles' pictures in the
 * PicoCalc's four greys, in NAME/NAME.I00, NAME.I01, ... (DESIGN.md 4.5)
 */
#ifndef PWG_PICTURES_H
#define PWG_PICTURES_H

#include "zim.h"
#include "pic.h"

#define PICS_NONE 0
#define PICS_LEAD 1		/* the first picture of each article */
#define PICS_ALL  2		/* all of them (up to PIC_MAX each) */

/* the largest picture: the text column's width, and a page's height less
 * room for two caption lines */
#define PWG_PIC_MAX_W 311
#define PWG_PIC_MAX_H 255	/* a page line is at most 255 high */

/* find, convert and write the pictures (on threads threads, each with
 * its own reader of input); base: OUTDIR/NAME/NAME.  Returns the number
 * of pictures written, or < 0 */
long pictures_make(zim_t *z, const char *input, const char *base, int mode, int threads);

/* the pictures of the article at entry (input numbering), as the HTML
 * parser and layout want them: hash, w, h, and off = picture number.
 * Thread safe once pictures_make has returned. */
void pictures_for(uint32_t entry, pic_set_t *s);

/* colour pictures (baseline JPEG records) instead of grey; quality 10
 * to 100 (lowered for a picture until it fits the PicoCalc's buffer) */
void pictures_colour(int on, int quality);

/* totals for the TOC header and the summary */
uint32_t pictures_count(void);

#endif
