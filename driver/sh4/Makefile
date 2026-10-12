KOS_WRAPPER ?= kos-cc
AR = sh-elf-ar
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
AFX_VALIDATE_ASSETS ?= 0
INCLUDES := -Iinclude -I../include -I../format/include
# The validation mode is part of flow's object name, so an object built for
# the other mode is never picked up.
FLOW_OBJ := build/flow.validate$(AFX_VALIDATE_ASSETS).o
HOST_OBJS := build/allocator.o build/dma.o build/instance.o build/ipc.o build/dsp_scene.o
OBJS := $(HOST_OBJS) $(FLOW_OBJ) build/dsp.o build/dsp_prefabs.o build/bank.o build/codec.o build/firmware.o
LIB := libaicaflow_host.a

all: $(LIB)

$(FLOW_OBJ): src/flow.c src/host_internal.h include/aicaflow/host.h ../include/aicaflow/protocol.h ../format/include/aicaflow/codec.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) -DAFX_VALIDATE_ASSETS=$(AFX_VALIDATE_ASSETS) $(INCLUDES) -c $< -o $@

# Which objects belong in the archive depends on the mode, which no timestamp
# records. Assemble it every time and replace it only when it differs, so an
# unchanged library keeps its date and nothing downstream relinks.
$(LIB): $(OBJS) FORCE
	@$(RM) $@.tmp
	@$(AR) rcsD $@.tmp $(OBJS)
	@if cmp -s $@.tmp $@; then $(RM) $@.tmp; else mv $@.tmp $@ && echo "$(AR) rcsD $@ $(OBJS)"; fi

FORCE:

$(HOST_OBJS): build/%.o: src/%.c src/host_internal.h include/aicaflow/host.h ../include/aicaflow/protocol.h ../format/include/aicaflow/codec.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/dsp.o: src/dsp.c include/aicaflow/dsp.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/dsp_prefabs.o: src/dsp_prefabs.c include/aicaflow/dsp.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/bank.o: src/bank.c include/aicaflow/bank.h include/aicaflow/host.h ../format/include/aicaflow/codec.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/codec.o: ../format/src/codec.c ../format/include/aicaflow/codec.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) build/flow.validate*.o $(LIB)

.PHONY: all clean FORCE

build/firmware.o: ../common/firmware.c ../include/aicaflow/firmware.h ../include/aicaflow/protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJS): $(wildcard ../format/include/aicaflow/*.h) ../include/aicaflow/firmware.h
