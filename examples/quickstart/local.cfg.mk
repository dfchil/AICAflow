ENJ_BASENAME := aicaflow_quickstart
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../driver/format/include -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

include ../authoring.mk
AUTHOR_DEMOS := $(AICAFORGE_BIN)/afx_demo_assets

$(ENJ_BUILDDIR)/fixture.afb $(ENJ_BUILDDIR)/fixture.afx &: $(AUTHOR_DEMOS)
	@mkdir -p $(@D)
	$(AUTHOR_DEMOS) quickstart $(ENJ_BUILDDIR)/fixture.afb $(ENJ_BUILDDIR)/fixture.afx

$(ENJ_BUILDDIR)/code/main.o: $(ENJ_BUILDDIR)/fixture.afb $(ENJ_BUILDDIR)/fixture.afx ../../firmware/aicaflow.drv
# pc-endjinn and web-endjinn (../../driver/host/host.mk) build in enDjinn's
# default directories, which are not defined yet when this file is read.
build/pc-endjinn/code/main.o build/web-endjinn/code/main.o: $(ENJ_BUILDDIR)/fixture.afb $(ENJ_BUILDDIR)/fixture.afx ../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C $(@D)
