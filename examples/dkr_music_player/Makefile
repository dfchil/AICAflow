ENJ_ROOT := $(patsubst %/base_link.mk,%,$(firstword $(wildcard ../../../enDjinn/base_link.mk ../../../../../enDjinn/base_link.mk)))
ifeq ($(ENJ_ROOT),)
$(error expected an enDjinn checkout beside AICAflow or the DKR checkout)
endif
include $(ENJ_ROOT)/base_link.mk
