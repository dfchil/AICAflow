ENJ_INJECT_QFONT := 1
ENJ_BASENAME := aicaflow_dynamic_sfx
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../format/include -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

$(ENJ_BUILDDIR)/code/main.o: ../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C $(@D)
