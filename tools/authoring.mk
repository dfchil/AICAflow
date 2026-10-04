# AICAforge native build. The monorepo supplies SRC=author FORMAT=../format.
# No runtime headers, sources, KOS, enDjinn or sibling checkout is required.
SRC ?= src
TEST ?= test
FORMAT ?= dependencies/aicaflow-format
BUILD ?= build
CC := clang
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
TEST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
export AFX_BUILD_DIR := $(abspath $(BUILD))
export PYTHONPATH := $(abspath research)
C_COMPILER := $(BUILD)/afx_compile
C_COMPILER_TEST := $(BUILD)/test_afx_compile
N64_CSEQ_TEST := $(BUILD)/test_afx_n64_cseq
N64_SFX_TEST := $(BUILD)/test_afx_n64_sfx
DEMO_ASSETS := $(BUILD)/afx_demo_assets
BANK_COMPILER := $(BUILD)/afx_bank
PROFILE_COMPILER := $(BUILD)/afx_profile
VGM_COMPILER := $(BUILD)/afx_vgm
N64_COMPILER := $(BUILD)/afx_n64

all: $(C_COMPILER) $(DEMO_ASSETS) $(BANK_COMPILER) $(PROFILE_COMPILER) $(VGM_COMPILER) $(N64_COMPILER)

check: all $(C_COMPILER_TEST) $(N64_CSEQ_TEST) $(N64_SFX_TEST)
	$(C_COMPILER_TEST)
	$(N64_CSEQ_TEST)
	$(N64_SFX_TEST)
	$(MAKE) -f $(TEST)/cli.mk BIN=$(abspath $(BUILD)) TEST=$(TEST) RESEARCH=research check
	@set -e; for test in $(filter-out $(TEST)/test_afx_tuner_client.py $(TEST)/test_afx_tuner_example.py,$(sort $(wildcard $(TEST)/test_*.py))); do python3 "$$test"; done

$(C_COMPILER): $(SRC)/afx_compile_c.c $(SRC)/afx_compile_c.h $(SRC)/afx_compile_c_cli.c $(SRC)/afx_midi_c.c $(SRC)/afx_midi_c.h $(SRC)/afx_sample_c.c $(SRC)/afx_sample_c.h $(SRC)/afx_sf2_c.c $(SRC)/afx_sf2_c.h $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c $(FORMAT)/include/aicaflow/codec.h $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(CFLAGS) -I$(FORMAT)/include $(SRC)/afx_compile_c.c $(SRC)/afx_midi_c.c $(SRC)/afx_sample_c.c $(SRC)/afx_sf2_c.c $(SRC)/afx_ya2beam.c $(SRC)/afx_compile_c_cli.c $(FORMAT)/src/codec.c -lm -o $@

$(C_COMPILER_TEST): $(SRC)/afx_compile_c.c $(SRC)/afx_compile_c.h $(SRC)/afx_midi_c.c $(SRC)/afx_midi_c.h $(SRC)/afx_sample_c.c $(SRC)/afx_sample_c.h $(SRC)/afx_ya2beam.c $(TEST)/test_afx_compile_c.c $(FORMAT)/src/codec.c $(FORMAT)/include/aicaflow/codec.h $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(TEST_CFLAGS) -I$(FORMAT)/include -I$(SRC) $(SRC)/afx_compile_c.c $(SRC)/afx_midi_c.c $(SRC)/afx_sample_c.c $(SRC)/afx_ya2beam.c $(TEST)/test_afx_compile_c.c $(FORMAT)/src/codec.c -lm -o $@

$(N64_CSEQ_TEST): $(SRC)/afx_n64_cseq.c $(SRC)/afx_n64_cseq.h $(SRC)/afx_compile_c.h $(TEST)/test_afx_n64_cseq.c $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(TEST_CFLAGS) -I$(FORMAT)/include -I$(SRC) $(SRC)/afx_n64_cseq.c $(TEST)/test_afx_n64_cseq.c -o $@

$(N64_SFX_TEST): $(TEST)/test_afx_n64_sfx.c $(N64_COMPILER)
	mkdir -p "$(BUILD)"
	$(CC) $(TEST_CFLAGS) -I$(FORMAT)/include -I$(SRC) $(TEST)/test_afx_n64_sfx.c $(SRC)/afx_n64_cseq.c $(SRC)/afx_compile_c.c $(SRC)/afx_sample_c.c $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c -lm -o $@

$(DEMO_ASSETS): $(SRC)/afx_demo_assets.c $(SRC)/afx_compile_c.c $(SRC)/afx_compile_c.h $(FORMAT)/src/codec.c $(FORMAT)/include/aicaflow/codec.h $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(CFLAGS) -I$(FORMAT)/include $(SRC)/afx_demo_assets.c $(SRC)/afx_compile_c.c $(FORMAT)/src/codec.c -lm -o $@

$(BANK_COMPILER): $(SRC)/afx_bank_c.c $(SRC)/afx_compile_c.c $(SRC)/afx_compile_c.h $(SRC)/afx_midi_c.c $(SRC)/afx_midi_c.h $(SRC)/afx_sample_c.c $(SRC)/afx_sample_c.h $(SRC)/afx_sf2_c.c $(SRC)/afx_sf2_c.h $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c $(FORMAT)/include/aicaflow/codec.h $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(CFLAGS) -I$(FORMAT)/include $(SRC)/afx_bank_c.c $(SRC)/afx_compile_c.c $(SRC)/afx_midi_c.c $(SRC)/afx_sample_c.c $(SRC)/afx_sf2_c.c $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c -lm -o $@

$(PROFILE_COMPILER): $(SRC)/afx_profile_c.c $(FORMAT)/src/codec.c $(FORMAT)/include/aicaflow/codec.h $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(CFLAGS) -I$(FORMAT)/include $(SRC)/afx_profile_c.c $(FORMAT)/src/codec.c -o $@

$(VGM_COMPILER): $(SRC)/afx_vgm.c $(SRC)/afx_compile_c.c $(SRC)/afx_compile_c.h $(SRC)/afx_sample_c.c $(SRC)/afx_sample_c.h $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c $(FORMAT)/include/aicaflow/codec.h $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(CFLAGS) -I$(FORMAT)/include -I$(SRC) $(SRC)/afx_vgm.c $(SRC)/afx_compile_c.c $(SRC)/afx_sample_c.c $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c -lm -lz -o $@

$(N64_COMPILER): $(SRC)/afx_n64.c $(SRC)/afx_n64_cseq.c $(SRC)/afx_n64_cseq.h $(SRC)/afx_compile_c.c $(SRC)/afx_compile_c.h $(SRC)/afx_sample_c.c $(SRC)/afx_sample_c.h $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c $(FORMAT)/include/aicaflow/codec.h $(FORMAT)/include/aicaflow/format.h
	mkdir -p "$(BUILD)"
	$(CC) $(CFLAGS) -I$(FORMAT)/include -I$(SRC) $(SRC)/afx_n64.c $(SRC)/afx_n64_cseq.c $(SRC)/afx_compile_c.c $(SRC)/afx_sample_c.c $(SRC)/afx_ya2beam.c $(FORMAT)/src/codec.c -lm -o $@

$(C_COMPILER) $(C_COMPILER_TEST) $(N64_CSEQ_TEST) $(N64_SFX_TEST) $(DEMO_ASSETS) $(BANK_COMPILER) $(PROFILE_COMPILER) $(VGM_COMPILER) $(N64_COMPILER): $(wildcard $(FORMAT)/include/aicaflow/*.h)

.PHONY: all check
