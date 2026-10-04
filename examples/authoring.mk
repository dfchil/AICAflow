# Examples consume authoring executables, never authoring source directly.
# Set an absolute AICAFORGE_BIN to use a standalone AICAforge build.
ifeq ($(origin AICAFORGE_BIN),undefined)
AICAFORGE_BIN := ../../build
$(addprefix $(AICAFORGE_BIN)/,afx_compile afx_bank afx_profile afx_demo_assets) &: $(wildcard ../../tools/author/*.[ch] ../../format/src/*.c ../../format/include/aicaflow/*.h) ../../tools/authoring.mk
	$(MAKE) -C ../.. compiler
endif
