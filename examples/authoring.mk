# Examples consume authoring executables, never authoring source directly.
# Set an absolute AICAFORGE_BIN to use a standalone AICAforge build.
ifeq ($(origin AICAFORGE_BIN),undefined)
AICAFORGE_BIN := ../../build
.PHONY: aicaforge-tools
aicaforge-tools:
	$(MAKE) -C ../.. compiler
$(addprefix $(AICAFORGE_BIN)/,afx_compile afx_bank afx_profile afx_demo_assets): aicaforge-tools
	@test -x "$@"
endif
