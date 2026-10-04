# Default examples use the pinned toolchain; explicit external binaries remain supported.
ifeq ($(origin AICAFORGE_BIN),undefined)
AICAFORGE_BIN ?= $(abspath ../../dependencies/AICAforge/build)
.PHONY: aicaforge-tools
aicaforge-tools:
	$(MAKE) -C ../.. authoring
$(addprefix $(AICAFORGE_BIN)/,afx_compile afx_bank afx_profile afx_demo_assets): aicaforge-tools
	@test -x "$@"
else
$(addprefix $(AICAFORGE_BIN)/,afx_compile afx_bank afx_profile afx_demo_assets):
	@echo "Missing $@. Build AICAforge and set AICAFORGE_BIN=/absolute/path/to/build" >&2
	@exit 1
endif
