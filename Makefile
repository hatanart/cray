# cray: RAY video poker emulator in C
#
#   make            cray (Raspberry Pi / Linux console build: DRM, ALSA, evdev, GPIO)
#   make sdl        cray-sdl (desktop window, for development)
#   make raytest    headless test tool
#
# Pi packages: sudo apt install build-essential libdrm-dev libasound2-dev

CC      ?= gcc
OPT     ?= -O2
CFLAGS  += $(OPT) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc -Imusashi \
           -DMUSASHI_CNF='"ray_conf.h"'
# Musashi is old code; build it quietly
MCFLAGS  = $(OPT) -Imusashi -DMUSASHI_CNF='"ray_conf.h"' -w

# one build folder per processor type, so a folder copied from another
# machine (e.g. a PC) never mixes its object files in
BUILD   = build/$(shell uname -m)
CORE    = machine acrtc pia coin ay video roms session
CORE_O  = $(CORE:%=$(BUILD)/%.o)
CPU_O   = $(BUILD)/m68kcpu.o $(BUILD)/m68kops.o $(BUILD)/softfloat.o

all: cray

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/ray.h src/session.h musashi/ray_conf.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m68kcpu.o: musashi/m68kcpu.c musashi/ray_conf.h | $(BUILD)
	$(CC) $(MCFLAGS) -c $< -o $@
$(BUILD)/m68kops.o: musashi/m68kops.c musashi/ray_conf.h | $(BUILD)
	$(CC) $(MCFLAGS) -c $< -o $@
$(BUILD)/softfloat.o: musashi/softfloat/softfloat.c | $(BUILD)
	$(CC) $(MCFLAGS) -c $< -o $@

$(BUILD)/libray.a: $(CORE_O) $(CPU_O)
	$(AR) rcs $@ $^

cray: pi/cray.c src/ray.h src/session.h $(BUILD)/libray.a
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libdrm alsa) pi/cray.c $(BUILD)/libray.a \
	    -o $@ $(shell pkg-config --libs libdrm alsa) -lm -lpthread

sdl: cray-sdl
cray-sdl: sdl/cray-sdl.c src/ray.h src/session.h $(BUILD)/libray.a
	$(CC) $(CFLAGS) $(shell pkg-config --cflags sdl2) sdl/cray-sdl.c $(BUILD)/libray.a \
	    -o $@ $(shell pkg-config --libs sdl2) -lm -lpthread

raytest: tools/raytest.c src/ray.h $(BUILD)/libray.a
	$(CC) $(CFLAGS) tools/raytest.c $(BUILD)/libray.a -o $@ -lm -lpthread

# ELPS/PAINT test: the core again with -DACRTC_TEST, in its own folder
TBUILD  = $(BUILD)-test
$(TBUILD):
	mkdir -p $(TBUILD)
$(TBUILD)/%.o: src/%.c src/ray.h src/session.h musashi/ray_conf.h | $(TBUILD)
	$(CC) $(CFLAGS) -DACRTC_TEST -c $< -o $@
elpstest: tools/elpstest.c src/ray.h $(CORE:%=$(TBUILD)/%.o) $(CPU_O)
	$(CC) $(CFLAGS) tools/elpstest.c $(CORE:%=$(TBUILD)/%.o) $(CPU_O) -o $@ -lm -lpthread

install: cray
	install -D -m 755 cray $(DESTDIR)/usr/local/bin/cray

clean:
	rm -rf build cray cray-sdl raytest elpstest

.PHONY: all sdl install clean
