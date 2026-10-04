include ../../dependencies/enDjinn/base_link.mk

GRIEG_DEFAULT_MIDI := sources/grieg-mountain-king-orchestra.mid
BACH_PERFORMANCE_MIDI := performance/bach-bwv1007-prelude-performance.mid
BACH_PERFORMANCE_SHA256 := 3455b959b496302bd784cde858c81808712c69c0af84b3aae062866733071599
DEFAULT_SOURCES := sources/chopin-op27-no1.mid $(BACH_PERFORMANCE_MIDI) \
	$(GRIEG_DEFAULT_MIDI)
GRIEG_MIDI ?= $(GRIEG_DEFAULT_MIDI)
DEFAULT_SOUNDFONT := sources/GeneralUser-GS.sf2
PIANO_SOUNDFONT ?= $(DEFAULT_SOUNDFONT)
CELLO_SOUNDFONT ?= sources/cello_solo.sf2
ORCHESTRA_SOUNDFONT ?= $(DEFAULT_SOUNDFONT)
CELLO_SF2_PROGRAM ?= 0
PIANO_RATE ?= 0
CELLO_RATE ?= 27000
ORCHESTRA_RATE ?= 0
PIANO_FORMAT ?= pcm16
CELLO_FORMAT ?= pcm16
PIANO_CHANNEL ?= stereo
CELLO_CHANNEL ?= stereo
ORCHESTRA_CHANNEL ?= left
ASSET_DIR := build/assets
BASE_DIR := build/base
MAP := build/classical.afbm
CONFIG := build/classical.config
ASSET_STAMP := $(ASSET_DIR)/.prepared
PROFILES := $(addprefix profiles/,chopin-op27-no1.afp bach-bwv1007-prelude.afp grieg-mountain-king.afp)
ASSETS := $(addprefix $(ASSET_DIR)/,chopin-op27-no1.afb chopin-op27-no1.afx chopin-op27-no1.afc chopin-op27-no1.afv \
	chopin-op27-no1.afi chopin-op27-no1.names.afi \
	bach-bwv1007-prelude.afb bach-bwv1007-prelude.afx bach-bwv1007-prelude.afc bach-bwv1007-prelude.afv \
	bach-bwv1007-prelude.afi bach-bwv1007-prelude.names.afi \
	grieg-mountain-king.afb grieg-mountain-king.afx grieg-mountain-king.afc grieg-mountain-king.afv \
	grieg-mountain-king.afi grieg-mountain-king.names.afi)
DISC := $(ENJ_ROMDIR)/$(ENJ_BASENAME)
DISC_STAMP := $(DISC)/.assets
FIRMWARE := $(DISC)/aicaflow.drv
AUTHOR := ../../build/afx_compile
BANK_AUTHOR := ../../build/afx_bank
PROFILE_AUTHOR := ../../build/afx_profile
# AFBM paths are relative to build/classical.afbm. Absolute user paths remain
# absolute; repository-relative defaults step out of build/ once.
map_path = $(if $(filter /%,$(1)),$(1),../$(1))

assets: $(ASSET_STAMP) $(FIRMWARE)

fetch-assets:
	sh fetch_assets.sh sources

$(AUTHOR) $(BANK_AUTHOR) $(PROFILE_AUTHOR): ../../tools/author/afx_compile_c.c ../../tools/author/afx_compile_c.h \
	../../tools/author/afx_compile_c_cli.c ../../tools/author/afx_midi_c.c \
	../../tools/author/afx_midi_c.h ../../tools/author/afx_sf2_c.c ../../tools/author/afx_sf2_c.h \
	../../tools/author/afx_bank_c.c ../../tools/author/afx_profile_c.c
	$(MAKE) -C ../.. compiler

FORCE:

$(CONFIG): FORCE
	@mkdir -p $(@D)
	@{ printf '%s\n' '$(PIANO_SOUNDFONT)' '$(CELLO_SOUNDFONT)' '$(ORCHESTRA_SOUNDFONT)' '$(GRIEG_MIDI)' '$(PIANO_FORMAT)' '$(CELLO_FORMAT)' '$(PIANO_CHANNEL)' '$(CELLO_CHANNEL)' '$(ORCHESTRA_CHANNEL)' '$(CELLO_SF2_PROGRAM)' '$(PIANO_RATE)' '$(CELLO_RATE)' '$(ORCHESTRA_RATE)' 'performance/bach-bwv1007-prelude-performance.mid'; } > "$@.tmp"
	@cmp -s "$@.tmp" "$@" 2>/dev/null || mv "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(MAP): Makefile classical.afbm $(CONFIG) $(BACH_PERFORMANCE_MIDI) fetch-assets
	@mkdir -p $(@D)
	@test -f "$(PIANO_SOUNDFONT)" -a -f "$(CELLO_SOUNDFONT)" -a -f "$(ORCHESTRA_SOUNDFONT)" -a -f "$(GRIEG_MIDI)" || { echo 'Missing selected MIDI or SoundFont input' >&2; exit 1; }
	@test "$$(shasum -a 256 "$(BACH_PERFORMANCE_MIDI)" | awk '{print $$1}')" = "$(BACH_PERFORMANCE_SHA256)" || { echo 'Bach performance MIDI does not match its checked-in source hash' >&2; exit 1; }
	@sed -e 's|@PIANO_SOUNDFONT@|$(call map_path,$(PIANO_SOUNDFONT))|g' \
		-e 's|@CELLO_SOUNDFONT@|$(call map_path,$(CELLO_SOUNDFONT))|g' \
		-e 's|@ORCHESTRA_SOUNDFONT@|$(call map_path,$(ORCHESTRA_SOUNDFONT))|g' \
		-e 's|@PIANO_FORMAT@|$(PIANO_FORMAT)|g' \
		-e 's|@CELLO_FORMAT@|$(CELLO_FORMAT)|g' \
		-e 's|@PIANO_CHANNEL@|$(PIANO_CHANNEL)|g' \
		-e 's|@CELLO_CHANNEL@|$(CELLO_CHANNEL)|g' \
		-e 's|@ORCHESTRA_CHANNEL@|$(ORCHESTRA_CHANNEL)|g' \
		-e 's|@CELLO_SF2_PROGRAM@|$(CELLO_SF2_PROGRAM)|g' \
		-e 's|@PIANO_RATE@|$(PIANO_RATE)|g' \
		-e 's|@CELLO_RATE@|$(CELLO_RATE)|g' \
		-e 's|@ORCHESTRA_RATE@|$(ORCHESTRA_RATE)|g' \
		-e 's|@GRIEG_MIDI@|$(call map_path,$(GRIEG_MIDI))|g' classical.afbm > "$@"

$(ASSET_STAMP): Makefile $(MAP) $(PROFILES) $(DEFAULT_SOURCES) $(DEFAULT_SOUNDFONT) $(BANK_AUTHOR) $(PROFILE_AUTHOR) ../../tools/author/afx_sample_c.c ../../tools/author/afx_ya2beam.c
	@mkdir -p $(ASSET_DIR) $(BASE_DIR) build/include
	$(BANK_AUTHOR) --per-song "$(MAP)" "$(BASE_DIR)"
	@set -e; for name in chopin-op27-no1 bach-bwv1007-prelude grieg-mountain-king; do \
		profile="profiles/$$name.afp"; use_profile=0; \
		if $(PROFILE_AUTHOR) describe "$(BASE_DIR)/$$name.afx" "$$profile" >/dev/null 2>&1; then use_profile=1; fi; \
		if [ $$use_profile = 1 ]; then $(PROFILE_AUTHOR) apply "$(BASE_DIR)/$$name.afx" "$(BASE_DIR)/$$name.afc" "$$profile" "$(ASSET_DIR)/$$name.afx" "$(ASSET_DIR)/$$name.afc"; else cp "$(BASE_DIR)/$$name.afx" "$(BASE_DIR)/$$name.afc" "$(ASSET_DIR)/"; fi; \
		$(AUTHOR) --visual "$(ASSET_DIR)/$$name.afx" "$(ASSET_DIR)/$$name.afv"; \
		cp "$(BASE_DIR)/$$name.afb" "$(BASE_DIR)/$$name.afi" "$(BASE_DIR)/$$name.names.afi" "$(ASSET_DIR)/"; \
	done
	@set -e; { \
		printf '%s\n' 'static const struct song { const char *title, *file, *bank, *dsp; bool wraps; uint32_t bytes, sample_bytes, stream_bytes, setup_bytes, padding_bytes, command_baseline_bytes, note_count; uint16_t sample_count, setup_count, tempo; uint8_t channel_count; } songs[] = {'; \
		for entry in 'chopin-op27-no1|Chopin - Nocturne Op. 27 No. 1' 'bach-bwv1007-prelude|Bach - Cello Suite 1 Prelude' 'grieg-mountain-king|Grieg - Hall of the Mountain King'; do \
			name=$${entry%%|*}; title=$${entry#*|}; set -- $$($(AUTHOR) --summary "$(ASSET_DIR)/$$name.afb" "$(ASSET_DIR)/$$name.afx"); \
			bytes=$$1; samples=$$2; stream=$$3; setup=$$4; notes=$$5; setups=$$6; channels=$$7; \
			profile="profiles/$$name.afp"; use_profile=0; \
			if $(PROFILE_AUTHOR) describe "$(BASE_DIR)/$$name.afx" "$$profile" >/dev/null 2>&1; then use_profile=1; fi; \
			if [ $$use_profile = 1 ]; then set -- $$($(PROFILE_AUTHOR) describe "$(BASE_DIR)/$$name.afx" "$$profile"); else set -- dry 0 256; fi; dsp=$$1; tempo=$$3; \
			if [ "$$dsp" = dry ]; then dsp=NULL; else dsp="\"$$dsp\""; fi; \
			printf '{"%s","%s.afx","%s.afb",%s,false,%s,%s,%s,%s,0,0,%s,%s,%s,%s,%s},\n' "$$title" "$$name" "$$name" "$$dsp" "$$bytes" "$$samples" "$$stream" "$$setup" "$$notes" "$$setups" "$$setups" "$$tempo" "$$channels"; \
		done; \
		printf '%s\n' '};'; \
	} > build/include/songs.h
	touch $@

build/include/songs.h: $(ASSET_STAMP)

$(DISC_STAMP): $(ASSET_STAMP)
	@mkdir -p $(DISC)
	@rm -f "$(DISC)/classical.afb"
	cp $(ASSETS) $(DISC)/
	touch $@

$(FIRMWARE): ../../firmware/aicaflow.drv
	@mkdir -p $(@D)
	cp $< $@

$(ENJ_BUILDDIR)/code/main.o: build/include/songs.h ../player_framework/music_player.c $(ASSET_STAMP) ../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: $(DISC_STAMP) $(FIRMWARE) ../../driver/sh4/libaicaflow_host.a
$(ENJ_BINDIR)/$(ENJ_BASENAME).cdi: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf $(DISC_STAMP) $(FIRMWARE)

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C $(@D)

../../driver/build/afx_validate:
	$(MAKE) -C ../../driver smoke

.PHONY: assets fetch-assets check frame-test FORCE

check: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf $(DISC_STAMP) ../../driver/build/afx_validate
	@for name in chopin-op27-no1 bach-bwv1007-prelude grieg-mountain-king; do \
		for extension in afb afx afc afv afi names.afi; do test -s "$(ASSET_DIR)/$$name.$$extension"; done; \
	done
	@for flow in $(ASSET_DIR)/*.afx; do ../../driver/build/afx_validate "$$flow"; done
	@python3 tests/filter_bounds.py $(ASSET_DIR)/*.afx
	@set -e; \
		for expected in 'chopin-op27-no1 1714 57' 'bach-bwv1007-prelude 1312 16' 'grieg-mountain-king 9352 57'; do \
			set -- $$expected; name=$$1; notes=$$2; channels=$$3; \
			set -- $$($(AUTHOR) --summary "$(ASSET_DIR)/$$name.afb" "$(ASSET_DIR)/$$name.afx"); \
			test "$$5" = "$$notes" -a "$$7" = "$$channels"; \
		done
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; mkdir "$$task_tmp/out"; \
		$(BANK_AUTHOR) --create-map "$$task_tmp/generated.afbm" "$(PIANO_SOUNDFONT)" generated sources/chopin-op27-no1.mid && \
		$(BANK_AUTHOR) "$$task_tmp/generated.afbm" "$$task_tmp/out" "$$task_tmp/generated.afb" && \
		test -s "$$task_tmp/generated.afb" -a -s "$$task_tmp/out/generated.afx"

# Builds the production player with a deterministic input script.  It uploads
# and plays every song through the normal asynchronous AFB/AFV loader, then
# prints a PASS line and exits.  Run ordinary `make` afterwards before a
# listening session: this target deliberately leaves a self-terminating ELF.
frame-test:
	$(MAKE) -B $(ENJ_BINDIR)/$(ENJ_BASENAME).elf 'ENJ_CFLAGS+= -DPLAYER_FRAME_TEST'
