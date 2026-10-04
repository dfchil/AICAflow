ENJ_INJECT_QFONT := 1
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../format/include -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

DSP_INPUT_DIR := $(ENJ_ROMDIR)/$(ENJ_BASENAME)
DSP_INPUTS := $(addprefix $(DSP_INPUT_DIR)/,effect.afb effect.afx impulse.afb impulse.afx tone.afb tone.afx modulated.afb modulated.afx wilhelm.afb wilhelm.afx)
DSP_INPUT_STAMP := $(DSP_INPUT_DIR)/.dsp_inputs
AUTHOR_DEMOS := ../../build/afx_demo_assets

assets: $(DSP_INPUTS)
all: assets
$(ENJ_BINDIR)/$(ENJ_BASENAME).cdi: assets

$(AUTHOR_DEMOS): ../../tools/author/afx_demo_assets.c ../../tools/author/afx_compile_c.c ../../tools/author/afx_compile_c.h ../../format/src/codec.c ../../format/include/aicaflow/codec.h
	$(MAKE) -C ../.. compiler

$(DSP_INPUT_STAMP): sources/wilhelm_scream.pcm $(AUTHOR_DEMOS)
	@mkdir -p $(@D)
	$(AUTHOR_DEMOS) dsp-effects $(DSP_INPUT_DIR) sources/wilhelm_scream.pcm
	touch $@

$(DSP_INPUTS): $(DSP_INPUT_STAMP)

$(ENJ_BUILDDIR)/code/main.o: ../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C ../../driver/sh4
