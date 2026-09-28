ENJ_INJECT_QFONT := 1
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

AICA_FIRMWARE := $(ENJ_ROMDIR)/$(ENJ_BASENAME)/aicaflow.drv
DKR_DISC := $(ENJ_ROMDIR)/$(ENJ_BASENAME)
DKR_ASSET_STAMP := $(DKR_DISC)/.assets
DKR_ASSETS := $(shell find assets -type f)

assets: $(DKR_ASSET_STAMP) $(AICA_FIRMWARE)

$(DKR_ASSET_STAMP): $(DKR_ASSETS)
	mkdir -p $(DKR_DISC)
	cp -R assets/. $(DKR_DISC)/
	touch $@

$(AICA_FIRMWARE): ../../driver/arm7/aicaflow.drv
	mkdir -p $(@D)
	cp $< $@

../../driver/arm7/aicaflow.drv ../../driver/sh4/libaicaflow_host.a: FORCE
	$(MAKE) -C $(@D)

$(ENJ_BUILDDIR)/code/main.o: include/songs.h ../music_player/player.c
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a | $(AICA_FIRMWARE)
$(ENJ_BINDIR)/$(ENJ_BASENAME).cdi: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf $(AICA_FIRMWARE)

FORCE:
.PHONY: FORCE check

check: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf $(DKR_ASSET_STAMP)
	test $$(find assets -maxdepth 1 -name '*.afx' | wc -l) -eq 64
	test $$(find assets -maxdepth 1 -name '*.afc' | wc -l) -eq 64
	test $$(find assets/music_visuals -name '*.viz' | wc -l) -eq 64
