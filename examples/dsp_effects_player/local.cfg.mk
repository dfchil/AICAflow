ENJ_INJECT_QFONT := 1
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

DSP_INPUT_DIR := $(ENJ_ROMDIR)/$(ENJ_BASENAME)
DSP_INPUTS := $(addprefix $(DSP_INPUT_DIR)/,effect.afb effect.afc impulse.afb impulse.afc tone.afb tone.afc modulated.afb modulated.afc wilhelm.afb wilhelm.afc)
DSP_INPUT_STAMP := $(DSP_INPUT_DIR)/.dsp_inputs

assets: $(DSP_INPUTS)
all: assets
$(ENJ_BINDIR)/$(ENJ_BASENAME).cdi: assets

$(DSP_INPUT_STAMP): prepare.py sources/wilhelm_scream.pcm ../../tools/afx_compile.py ../../tools/afx_midi.py ../../tools/afx_music_bank.py
	python3 prepare.py $(DSP_INPUT_DIR) sources/wilhelm_scream.pcm
	touch $@

$(DSP_INPUTS): $(DSP_INPUT_STAMP)

$(ENJ_BUILDDIR)/code/main.o: ../../driver/arm7/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C ../../driver/sh4
