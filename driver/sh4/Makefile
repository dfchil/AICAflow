KOS_WRAPPER ?= kos-cc
AR = sh-elf-ar
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
INCLUDES := -Iinclude -I../include -I../../format/include
HOST_OBJS := build/allocator.o build/dma.o build/flow.o build/instance.o build/ipc.o build/dsp_scene.o
OBJS := $(HOST_OBJS) build/dsp.o build/dsp_prefabs.o build/bank.o build/codec.o build/firmware.o
LIB := libaicaflow_host.a

all: $(LIB)

$(LIB): $(OBJS)
	$(RM) $@
	$(AR) rcs $@ $(OBJS)

$(HOST_OBJS): build/%.o: src/%.c src/host_internal.h include/aicaflow/host.h ../include/aicaflow/protocol.h ../../format/include/aicaflow/codec.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/dsp.o: src/dsp.c include/aicaflow/dsp.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/dsp_prefabs.o: src/dsp_prefabs.c include/aicaflow/dsp.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/bank.o: src/bank.c include/aicaflow/bank.h include/aicaflow/host.h ../../format/include/aicaflow/codec.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/codec.o: ../../format/src/codec.c ../../format/include/aicaflow/codec.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean

build/firmware.o: ../common/firmware.c ../include/aicaflow/firmware.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJS): $(wildcard ../../format/include/aicaflow/*.h) ../include/aicaflow/firmware.h
