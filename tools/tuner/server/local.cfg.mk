ENJ_BASENAME := afx_tuner_server
ENJ_CODEDIR := .
ENJ_CBASEPATH := /pc
ENJ_INJECT_QFONT := 1
ENJ_KOS_INIT_FLAGS := (INIT_DEFAULT | INIT_NET)
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -I../../../format/include -I../../../driver/include -I../../../driver/sh4/include
ENJ_LDLIBS += ../../../driver/sh4/libaicaflow_host.a
DEFINES += -DENJ_KOS_INIT_FLAGS='$(ENJ_KOS_INIT_FLAGS)'
$(ENJ_BUILDDIR)/main.o: ../../../firmware/aicaflow.drv
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: ../../../driver/sh4/libaicaflow_host.a

../../../driver/sh4/libaicaflow_host.a: FORCE
	$(MAKE) -C $(@D)

FORCE:
.PHONY: FORCE
