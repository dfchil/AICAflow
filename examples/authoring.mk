# Use a separately built AICAforge checkout; never build a local compiler copy.
AICAFORGE_BIN ?= $(abspath ../../dependencies/AICAforge/build)
$(addprefix $(AICAFORGE_BIN)/,afx_compile afx_bank afx_profile afx_demo_assets):
	@echo "Missing $@. Build AICAforge and set AICAFORGE_BIN=/absolute/path/to/build" >&2
	@exit 1
