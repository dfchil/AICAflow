KOS_WRAPPER ?= kos-cc
PYTHON ?= python3
TARGET := bin/aicaflow_dsp_demo.elf
OBJECT := build/main.o
FLOW := build/demo.afx
MIDI := build/demo.mid
INCLUDES := -I../../driver/include -I../../driver/sh4/include
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu23

.PHONY: all clean

all: $(TARGET)

$(MIDI): ../../tools/make_dsp_demo_midi.py
	@mkdir -p $(@D)
	$(PYTHON) $< $@

$(FLOW): $(MIDI) mapping.json ../../tools/afx_compile.py ../../tools/afx_midi.py
	$(PYTHON) ../../tools/afx_compile.py $(MIDI) mapping.json $@

$(OBJECT): code/main.c $(FLOW) ../../driver/arm7/aicaflow.drv
	@mkdir -p $(@D)
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C ../../driver/sh4

$(TARGET): $(OBJECT) ../../driver/sh4/libaicaflow_host.a
	@mkdir -p $(@D)
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -o $@ $(OBJECT) ../../driver/sh4/libaicaflow_host.a

clean:
	rm -rf build bin
