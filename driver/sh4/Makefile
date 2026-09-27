KOS_WRAPPER ?= kos-cc
AR = sh-elf-ar
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
INCLUDES := -Iinclude -I../include
OBJS := build/host.o build/dsp.o build/sfx_bank.o build/codec.o
LIB := libaicaflow_host.a

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

build/host.o: src/host.c include/aicaflow/host.h ../include/aicaflow_protocol.h ../include/aicaflow_codec.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/dsp.o: src/dsp.c include/aicaflow/dsp.h ../include/aicaflow_protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/sfx_bank.o: src/sfx_bank.c include/aicaflow/sfx_bank.h include/aicaflow/host.h ../include/aicaflow_protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/codec.o: ../common/codec.c ../include/aicaflow_codec.h ../include/aicaflow_protocol.h
	@mkdir -p build
	$(KOS_WRAPPER) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
