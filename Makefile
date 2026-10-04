SHELL := /bin/bash

KOS_ENV ?= /opt/toolchains/dc/kos/environ.sh
EXAMPLES := quickstart multiple_dsp_effects dynamic_sfx dsp_effects_player music_player
TOOLS := tuner/server
C_COMPILER := build/afx_compile
C_COMPILER_TEST := build/test_afx_compile
N64_CSEQ_TEST := build/test_afx_n64_cseq
N64_SFX_TEST := build/test_afx_n64_sfx
DEMO_ASSETS := build/afx_demo_assets
BANK_COMPILER := build/afx_bank
PROFILE_COMPILER := build/afx_profile
VGM_COMPILER := build/afx_vgm
N64_COMPILER := build/afx_n64

.PHONY: all examples tools check compiler firmware firmware-check clean

all: examples tools

examples:
	@for example in $(EXAMPLES); do \
		source $(KOS_ENV) && $(MAKE) -C examples/$$example || exit $$?; \
	done

tools:
	@for tool in $(TOOLS); do \
		source $(KOS_ENV) && $(MAKE) -C tools/$$tool || exit $$?; \
	done

runtime-check: format-check
	$(MAKE) -C driver check
	python3 driver/tests/test_compatibility.py

authoring-check:
	$(MAKE) -C tools -f authoring.mk SRC=author FORMAT=../format BUILD=../build check

# An external AICAforge build may be selected without compiling the old copy.
AICAFORGE_BIN ?= $(CURDIR)/build
compatibility-check:
	$(MAKE) -C driver build/afx_validate build/test_bank
	$(MAKE) -f tools/test/cli.mk BIN="$(AICAFORGE_BIN)" TEST=tools/test RESEARCH=tools/research VALIDATOR="$(CURDIR)/driver/build/afx_validate" check
	python3 driver/tests/test_compatibility.py --authoring-bin "$(AICAFORGE_BIN)"

check:
	$(MAKE) runtime-check
	$(MAKE) authoring-check
	$(MAKE) compatibility-check

compiler:
	$(MAKE) -C tools -f authoring.mk SRC=author FORMAT=../format BUILD=../build all

firmware:
	source $(KOS_ENV) && $(MAKE) -C driver/arm7

firmware-check: firmware
	@expected=$$(python3 -c 'import json; print(json.load(open("firmware/manifest.json"))["sha256"])'); \
	actual=$$(shasum -a 256 firmware/aicaflow.drv | awk '{print $$1}'); \
	test "$$actual" = "$$expected" || { echo "firmware/aicaflow.drv differs from its release manifest" >&2; exit 1; }

clean:
	@for example in $(EXAMPLES); do $(MAKE) -C examples/$$example clean; done
	@for tool in $(TOOLS); do $(MAKE) -C tools/$$tool clean; done
	$(MAKE) -C driver clean
	rm -f $(C_COMPILER) $(C_COMPILER_TEST) $(DEMO_ASSETS) $(BANK_COMPILER) $(PROFILE_COMPILER) \
		$(VGM_COMPILER) $(N64_COMPILER) $(N64_CSEQ_TEST) $(N64_SFX_TEST) build/afx_compile_c build/test_afx_compile_c build/afx_bank_c build/afx_profile_c

format-check:
	$(MAKE) -C format check

.PHONY: format-check runtime-check authoring-check compatibility-check
