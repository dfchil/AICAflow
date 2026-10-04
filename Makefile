SHELL := /bin/bash

KOS_ENV ?= /opt/toolchains/dc/kos/environ.sh
EXAMPLES := quickstart multiple_dsp_effects dynamic_sfx dsp_effects_player music_player
TOOLS := tuner/server

.PHONY: all examples tools check firmware firmware-check clean

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

# AICAforge is an external checkout; ordinary runtime checks do not need it.
AICAFORGE_BIN ?= $(CURDIR)/dependencies/AICAforge/build
AICAFORGE_DIR ?= $(abspath $(AICAFORGE_BIN)/..)
compatibility-check:
	@test -x "$(AICAFORGE_BIN)/afx_demo_assets" || { echo "Build AICAforge and set AICAFORGE_BIN=/absolute/path/to/build" >&2; exit 1; }
	$(MAKE) -C driver build/afx_validate build/test_bank
	$(MAKE) -C "$(AICAFORGE_DIR)" -f test/cli.mk BIN="$(abspath $(AICAFORGE_BIN))" VALIDATOR="$(CURDIR)/driver/build/afx_validate" check
	python3 driver/tests/test_compatibility.py --authoring-bin "$(AICAFORGE_BIN)"

check: runtime-check

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

format-check:
	$(MAKE) -C format check

.PHONY: format-check runtime-check compatibility-check
