# AICAflow for an enDjinn application on pc-enDjinn or web-enDjinn: the SH4
# library and the real firmware on the software AICA, heard through enDjinn's
# audio output. Include it before base_link.mk:
#
#   include path/to/AICAflow/driver/host/host.mk
#   include $(ENJDIR)/base_link.mk
#
# Nothing changes for a Dreamcast build. See docs/design/host-backend.md.
AICAFLOW_HOST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
AICAFLOW_HOST_INCLUDES := -I$(AICAFLOW_HOST_DIR)/include -I$(AICAFLOW_HOST_DIR)/../sh4/include \
	-I$(AICAFLOW_HOST_DIR)/../include -I$(AICAFLOW_HOST_DIR)/../format/include

ifeq ($(ENJ_TARGET),pc-endjinn)
AICAFLOW_HOST_LIB := $(AICAFLOW_HOST_DIR)/libaicaflow_pc.a
AICAFLOW_HOST_GLUE := $(AICAFLOW_HOST_DIR)/build/pc/glue/afx_enj_audio.o
ENJ_HOST_CPPFLAGS += $(AICAFLOW_HOST_INCLUDES)
ENJ_HOST_EXTRA_OBJS += $(AICAFLOW_HOST_GLUE) $(AICAFLOW_HOST_LIB)
ENJ_LDFLAGS += -pthread
$(AICAFLOW_HOST_GLUE): $(AICAFLOW_HOST_DIR)/glue/afx_enj_audio.c $(AICAFLOW_HOST_DIR)/include/aicaflow/host_audio.h
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -I$(AICAFLOW_HOST_DIR)/include -I$(ENJDIR)include -c $< -o $@
else ifeq ($(ENJ_TARGET),web-endjinn)
AICAFLOW_HOST_LIB := $(AICAFLOW_HOST_DIR)/libaicaflow_web.a
AICAFLOW_HOST_GLUE := $(AICAFLOW_HOST_DIR)/build/web/glue/afx_enj_audio.o
ENJ_WEB_CPPFLAGS += $(AICAFLOW_HOST_INCLUDES)
ENJ_WEB_EXTRA_OBJS += $(AICAFLOW_HOST_GLUE) $(AICAFLOW_HOST_LIB)
$(AICAFLOW_HOST_GLUE): $(AICAFLOW_HOST_DIR)/glue/afx_enj_audio.c $(AICAFLOW_HOST_DIR)/include/aicaflow/host_audio.h
	@mkdir -p $(dir $@)
	$(EMCC) -std=gnu11 -O2 -Wall -Wextra -Werror -I$(AICAFLOW_HOST_DIR)/include -I$(ENJDIR)include -c $< -o $@
endif

ifneq ($(AICAFLOW_HOST_LIB),)
# Always ask the library's own Makefile: it knows its sources.
$(AICAFLOW_HOST_LIB): FORCE_AICAFLOW_HOST
	$(MAKE) -C $(AICAFLOW_HOST_DIR) $(notdir $@)
.PHONY: FORCE_AICAFLOW_HOST
FORCE_AICAFLOW_HOST:
endif
