ENJ_BASENAME := aicaflow_quickstart
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../format/include -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

AUTHOR_DEMOS := ../../build/afx_demo_assets

$(AUTHOR_DEMOS): ../../tools/author/afx_demo_assets.c ../../tools/author/afx_compile_c.c ../../tools/author/afx_compile_c.h ../../driver/common/codec.c ../../driver/include/aicaflow/codec.h
	$(MAKE) -C ../.. compiler

$(ENJ_BUILDDIR)/fixture.afb $(ENJ_BUILDDIR)/fixture.afx &: $(AUTHOR_DEMOS)
	@mkdir -p $(@D)
	$(AUTHOR_DEMOS) quickstart $(ENJ_BUILDDIR)/fixture.afb $(ENJ_BUILDDIR)/fixture.afx

$(ENJ_BUILDDIR)/code/main.o: $(ENJ_BUILDDIR)/fixture.afb $(ENJ_BUILDDIR)/fixture.afx ../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C $(@D)
