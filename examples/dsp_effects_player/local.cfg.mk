ENJ_INJECT_QFONT := 1
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

FLOW := build/dsp_effects.afx
MIDI := build/dsp_effects.mid

$(MIDI): ../../tools/make_dsp_effects_player_midi.py
	@mkdir -p $(@D)
	python3 $< $@

$(FLOW): $(MIDI) mapping.json ../../tools/afx_compile.py ../../tools/afx_midi.py
	python3 ../../tools/afx_compile.py $(MIDI) mapping.json $@

$(ENJ_BUILDDIR)/code/main.o: $(FLOW) ../../driver/arm7/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C ../../driver/sh4
