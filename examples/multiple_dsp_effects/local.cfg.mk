ENJ_BASENAME := aicaflow_multiple_dsp_effects
ENJ_INJECT_QFONT := 1
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a

AUTHOR_DEMOS := ../../build/afx_demo_assets

$(AUTHOR_DEMOS): ../../tools/author/afx_demo_assets.c ../../tools/author/afx_compile_c.c ../../tools/author/afx_compile_c.h ../../driver/common/codec.c ../../driver/include/aicaflow/codec.h ../../driver/include/aicaflow/protocol.h
	$(MAKE) -C ../.. compiler

$(ENJ_BUILDDIR)/dual.afb $(ENJ_BUILDDIR)/dual.afx &: $(AUTHOR_DEMOS)
	@mkdir -p $(@D)
	$(AUTHOR_DEMOS) multiple-dsp-effects $(ENJ_BUILDDIR)/dual.afb $(ENJ_BUILDDIR)/dual.afx

$(ENJ_BUILDDIR)/code/main.o: $(ENJ_BUILDDIR)/dual.afb $(ENJ_BUILDDIR)/dual.afx ../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../driver/sh4/libaicaflow_host.a

../../driver/sh4/libaicaflow_host.a:
	$(MAKE) -C $(@D)
