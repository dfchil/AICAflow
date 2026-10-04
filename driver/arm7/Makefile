# The current KOS environment also exports ARM_CC=marm for a different ARM
# target. AICA firmware must follow the explicit DC_ARM_* cross tools.
ARM_CC := $(DC_ARM_CC)
ARM_OBJCOPY := $(DC_ARM_OBJCOPY)
ARM_CFLAGS ?= -marm -mcpu=arm7di -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -std=c11
ARM_LDFLAGS ?= -nostdlib -Wl,-T,build/aicaflow.ld -Wl,--build-id=none -Wl,-Map,aicaflow.map
TARGET := ../../firmware/aicaflow.drv
ELF := aicaflow.elf
OBJS := build/crt0.o build/driver.o

all: $(TARGET)

$(TARGET): $(ELF)
	mkdir -p $(@D)
	$(ARM_OBJCOPY) -O binary $< $@

$(ELF): $(OBJS) build/aicaflow.ld
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) -o $@ $(OBJS)

build/aicaflow.ld: aicaflow.ld.S ../include/aicaflow/protocol.h | build
	$(ARM_CC) -E -P -x assembler-with-cpp -I../include -I../format/include $< -o $@

build/crt0.o: crt0.s ../include/aicaflow/protocol.h | build
	$(ARM_CC) $(ARM_CFLAGS) -x assembler-with-cpp -I../include -I../format/include -c -o $@ $<

build/driver.o: driver.c ../include/aicaflow/protocol.h ../format/include/aicaflow/codec.h | build
	$(ARM_CC) $(ARM_CFLAGS) -fstack-usage -I../include -I../format/include -c -o $@ $<

build:
	mkdir -p $@

clean:
	rm -f $(OBJS) build/driver.su build/aicaflow.ld $(ELF) aicaflow.map

.PHONY: all clean

$(OBJS) build/aicaflow.ld: $(wildcard ../format/include/aicaflow/*.h)
