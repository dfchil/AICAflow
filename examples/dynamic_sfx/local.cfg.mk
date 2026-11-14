ENJ_INJECT_QFONT := 1
ENJ_BASENAME := aicaflow_dynamic_sfx
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../driver/format/include -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

$(ENJ_BUILDDIR)/code/main.o: ../../firmware/aicaflow.drv
# pc-endjinn and web-endjinn (../../driver/host/host.mk) build in enDjinn's
# default directories, which are not defined yet when this file is read.
build/pc-endjinn/code/main.o build/web-endjinn/code/main.o: ../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C $(@D)
