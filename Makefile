# PicoWikiGuide PC build: zimprep, the PC versions of the app and the tests.
# The PicoCalc firmware is built with pico-sdk from pico/ (see README.md).

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -MMD -MP
BUILD   := build

# zimprep uses the full zstd library (compression and dictionary training)
ZSTD_FULL := $(wildcard third_party/zstd-full/common/*.c third_party/zstd-full/compress/*.c \
             third_party/zstd-full/decompress/*.c third_party/zstd-full/dictBuilder/*.c)
XZ        := third_party/xz/xz_crc32.c third_party/xz/xz_dec_stream.c third_party/xz/xz_dec_lzma2.c
PREP_SRC  := prep/zimprep.c prep/html.c prep/tex.c prep/zim.c prep/pic.c prep/layout.c \
             src/fold.c src/index.c src/font.c src/gfx.c fonts/fonts.c host/hostio.c
PREP_INC  := -Isrc -Iprep -Ihost -Ifonts -Ithird_party/zstd-full -Ithird_party/xz

# the app side (what runs on the PicoCalc), built for the PC
DEC       := third_party/zstd/zstddeclib.c
APP_CORE  := src/wiki.c src/index.c src/fold.c src/font.c src/gfx.c fonts/fonts.c
APP_INC   := -Isrc -Ihost -Ifonts -Ipico -Ithird_party/zstd

obj = $(patsubst %.c,$(BUILD)/$(2)/%.o,$(1))

APP_SRC   := src/app.c src/splash.c src/calc.c $(APP_CORE) host/hostio.c host/plat_files.c host/plat_common.c

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
APPS := $(BUILD)/pwg-headless $(BUILD)/pwg-imgtest
ifneq ($(SDL_LIBS),)
APPS += $(BUILD)/pwg-sdl
endif

all: $(BUILD)/zimprep $(BUILD)/pagecat $(APPS)

$(BUILD)/pwg-headless: $(call obj,host/plat_headless.c $(APP_SRC),app) $(call obj,$(DEC),dec)
	$(CC) -o $@ $^ -lm

# the app with the PicoCalc's file code, on a FAT card image (tests)
IMG_SRC := src/app.c src/splash.c src/calc.c $(APP_CORE) src/fat.c src/blockdev.c pico/plat_storage.c \
           host/sd_image.c host/hostio.c host/plat_common.c
$(BUILD)/pwg-imgtest: $(call obj,$(IMG_SRC),app) $(BUILD)/img/host/plat_headless.o $(call obj,$(DEC),dec)
	$(CC) -o $@ $^ -lm

$(BUILD)/img/host/plat_headless.o: host/plat_headless.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(APP_INC) -Ipico -DCARD_IMAGE -c $< -o $@

$(BUILD)/pwg-sdl: $(call obj,host/plat_sdl.c $(APP_SRC),app) $(call obj,$(DEC),dec)
	$(CC) -o $@ $^ $(SDL_LIBS) -lm

$(BUILD)/app/host/plat_sdl.o: host/plat_sdl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(APP_INC) $(SDL_CFLAGS) -c $< -o $@

$(BUILD)/pagecat: $(call obj,tests/pagecat.c $(APP_CORE) host/hostio.c host/plat_common.c,app) $(call obj,$(DEC),dec)
	$(CC) -o $@ $^

$(BUILD)/app/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(APP_INC) -c $< -o $@

$(BUILD)/dec/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -DZSTD_LEGACY_SUPPORT=0 -DZSTD_DISABLE_ASM -c $< -o $@

$(BUILD)/zimprep: $(call obj,$(PREP_SRC),prep) $(call obj,$(ZSTD_FULL) $(XZ),lib)
	$(CC) -o $@ $^ -lpthread

$(BUILD)/prep/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PREP_INC) -c $< -o $@

$(BUILD)/lib/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -DZSTD_DISABLE_ASM -DZSTD_MULTITHREAD=0 -Ithird_party/zstd-full -Ithird_party/xz -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

.PHONY: all clean
