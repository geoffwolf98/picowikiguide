/*
 * PicoWikiGuide - the card format shared by zimprep and the device.
 * See DESIGN.md section 4.  All numbers little-endian.
 */
#ifndef CARD_H
#define CARD_H

#include "zw.h"

#define TOC_MAGIC        0x54475750u	/* "PWGT" */
#define TOC_VERSION      1
#define TOC_HDR_SIZE     4096

/* must-understand flags this firmware knows (none yet) */
#define TOC_MUST_KNOWN   0u

/* content flags (informational) */
#define TOC_C_NOTES      1u
#define TOC_C_CITES      2u
#define TOC_C_FORMULA_BM 4u
#define TOC_C_PICTURES   8u

#define TOC_COMP_ZSTD_DICT 1

/* header field offsets */
enum {
	TOC_H_MAGIC = 0, TOC_H_VERSION = 4, TOC_H_HDRSIZE = 6, TOC_H_MUST = 8,
	TOC_H_CONTENT = 12, TOC_H_COMP = 16, TOC_H_PPF = 17, TOC_H_TEXTPX = 18,
	TOC_H_PIECE_SHIFT = 19, TOC_H_FONTS = 20, TOC_H_PAGE_W = 24,
	TOC_H_PAGE_H = 26, TOC_H_ARTICLES = 28, TOC_H_FRAMES = 32,
	TOC_H_PIECES = 36, TOC_H_MAIN = 40, TOC_H_DICT_OFF = 44,
	TOC_H_DICT_LEN = 48, TOC_H_ART_OFF = 52, TOC_H_FRAME_OFF = 56,
	TOC_H_SEC_OFF = 60, TOC_H_SEC_LEN = 64, TOC_H_DATA_LEN = 72,
	TOC_H_PREP = 80, TOC_H_TITLE = 96, TOC_H_LANG = 224, TOC_H_NAME = 240,
	TOC_H_DATE = 256, TOC_H_UUID = 272, TOC_H_PICS = 288,
	TOC_H_PREP_LEN = 16, TOC_H_TITLE_LEN = 128, TOC_H_LANG_LEN = 16,
	TOC_H_NAME_LEN = 16, TOC_H_DATE_LEN = 16,
};

#define TOC_ART_REC      12	/* u32 first frame, u16 pages, u16 sections, u32 section offset */
#define TOC_FRAME_REC    8	/* u40 data offset, u24 compressed length */
#define TOC_NONE         0xffffffffu

/* page geometry: a top bar, then the body that pages fill */
#define PAGE_W           320
#define TOPBAR_H         18
#define PAGE_TOP         (TOPBAR_H + 2)
#define PAGE_H           (320 - PAGE_TOP)
#define TEXT_PX          18

/* line decorations in a page (deco byte; bit 7 = quote bar) */
enum { PD_NONE, PD_BULLET, PD_NUMBER, PD_RULE, PD_HEAD_RULE };
#define PD_QUOTE         0x80

#define PI_EXT           0x80	/* item types >= this are skipped by v1 */
#define PI_PIC           0x81	/* a picture: i16 x, u16 w, u16 h, u32 number */

/* pictures (NAME.I00 ...; DESIGN.md 4.5) */
#define PIC_FILE_HDR     64	/* "PWGI", u32 version, u32 count, ... */
#define PIC_MAX_BYTES    (((311 * 2 + 7) / 8) * 255)	/* the largest, 2 bits a pixel */
#define PIC_ZMAX         16000	/* compressed records are at most this (+ 8) */
#define PI_NOLINK        0xff

#define FRAME_MAX        16384	/* largest decompressed frame */

#endif
