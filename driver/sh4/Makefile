KOS_WRAPPER ?= kos-cc
AR = sh-elf-ar
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
INCLUDES := -Iinclude -I../include
HOST_OBJS := build/allocator.o build/dma.o build/flow.o build/instance.o build/ipc.o build/dsp_scene.o
OBJS := $(HOST_OBJS) build/dsp.o build/sfx_bank.o build/codec.o
LIB := libaicaflow_host.a

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(HOST_OBJS): build/%.o: src/%.c src/host_internal.h include/aicaflow/host.h ../include/aicaflow/protocol.h ../include/aicaflow/codec.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/dsp.o: src/dsp.c include/aicaflow/dsp.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/sfx_bank.o: src/sfx_bank.c include/aicaflow/sfx_bank.h include/aicaflow/host.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/codec.o: ../common/codec.c ../include/aicaflow/codec.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
