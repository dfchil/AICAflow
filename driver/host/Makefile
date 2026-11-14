# AICAflow for a PC or a browser: the SH4 library and the firmware's executor
# (driver/arm7/driver.c, compiled natively) on a software AICA. See docs/design/host-backend.md.
#
#   make -C driver/host           libaicaflow_pc.a
#   make -C driver/host web       libaicaflow_web.a (needs emcc)
#   make -C driver/host check     host tests, including the console checks

# Clang is the default, as in driver/Makefile; CC from the environment or the
# command line selects another compiler.
ifeq ($(origin CC),default)
CC := clang
endif
EMCC ?= emcc
AFX_VALIDATE_ASSETS ?= 0
WARNINGS := -Wall -Wextra -Werror
CFLAGS ?= -O2 -g
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
INCLUDES := -Ishim -Iinclude -I../sh4/include -I../include -I../format/include
# The firmware source copies payloads through word pointers, as its ARM build
# allows; the host must not reorder those copies.
DEFS := -D_GNU_SOURCE -DAFX_VALIDATE_ASSETS=$(AFX_VALIDATE_ASSETS) -fno-strict-aliasing

LIB_SRCS := emu/aica.c src/firmware.c src/platform.c \
	../sh4/src/allocator.c ../sh4/src/bank.c ../sh4/src/dma.c ../sh4/src/dsp.c \
	../sh4/src/dsp_prefabs.c ../sh4/src/dsp_scene.c ../sh4/src/flow.c \
	../sh4/src/instance.c ../sh4/src/ipc.c ../sh4/src/scene.c \
	../common/firmware.c ../format/src/codec.c
LIB_HEADERS := $(wildcard emu/*.h src/*.h shim/*.h shim/*/*.h include/aicaflow/*.h ../sh4/src/*.h \
	../arm7/driver.c ../arm7/*.h \
	../sh4/include/aicaflow/*.h ../include/aicaflow/*.h ../format/include/aicaflow/*.h)

PC_BUILD := build/pc
WEB_BUILD := build/web
PC_OBJS := $(patsubst %.c,$(PC_BUILD)/%.o,$(subst ../,up/,$(LIB_SRCS)))
WEB_OBJS := $(patsubst %.c,$(WEB_BUILD)/%.o,$(subst ../,up/,$(LIB_SRCS)))

all: libaicaflow_pc.a afx_render
web: libaicaflow_web.a

afx_render: tools/afx_render.c libaicaflow_pc.a
	$(CC) -std=gnu11 $(CFLAGS) $(WARNINGS) -Iinclude -I../sh4/include -I../include -I../format/include \
		-DAFX_RENDER_FIRMWARE='"$(abspath ../../firmware/aicaflow.drv)"' $< libaicaflow_pc.a -pthread -lm -o $@

libaicaflow_pc.a: $(PC_OBJS)
	@rm -f $@
	ar rcs $@ $^

libaicaflow_web.a: $(WEB_OBJS)
	@rm -f $@
	emar rcs $@ $^

$(PC_BUILD)/up/%.o: ../%.c $(LIB_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 $(CFLAGS) $(WARNINGS) $(DEFS) $(INCLUDES) -pthread -c $< -o $@
$(PC_BUILD)/%.o: %.c $(LIB_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 $(CFLAGS) $(WARNINGS) $(DEFS) $(INCLUDES) -pthread -c $< -o $@
$(WEB_BUILD)/up/%.o: ../%.c $(LIB_HEADERS)
	@mkdir -p $(dir $@)
	$(EMCC) -std=gnu11 $(CFLAGS) $(WARNINGS) $(DEFS) $(INCLUDES) -c $< -o $@
$(WEB_BUILD)/%.o: %.c $(LIB_HEADERS)
	@mkdir -p $(dir $@)
	$(EMCC) -std=gnu11 $(CFLAGS) $(WARNINGS) $(DEFS) $(INCLUDES) -c $< -o $@

# Tests build the library sources directly, with the sanitizers.
TEST_BUILD := build/test
TEST_CFLAGS := -std=gnu2x -O2 -g $(WARNINGS) $(SANITIZE) $(DEFS) -pthread
EXPAND := python3 tests/expand_embed.py
CHECKS := check_song check_commands check_banks check_scenes check_timer

$(TEST_BUILD)/libaicaflow_test.a: $(LIB_SRCS) $(LIB_HEADERS)
	@mkdir -p $(TEST_BUILD)/lib
	cd $(TEST_BUILD)/lib && rm -f *.o && for src in $(LIB_SRCS); do \
		$(CC) -std=gnu11 -O2 -g $(WARNINGS) $(SANITIZE) $(DEFS) -pthread \
			$(addprefix -I$(CURDIR)/,$(patsubst -I%,%,$(INCLUDES))) \
			-c $(CURDIR)/$$src -o $$(echo $$src | tr '/.' '__').o || exit 1; done
	ar rcs $@ $(TEST_BUILD)/lib/*.o

FIXTURES := ../build/long.afx ../build/long.afb ../build/song.afm ../build/song_new.afm
$(FIXTURES): ../tests/make_fixtures.py ../tests/fixtures/quickstart.afx.hex ../tests/fixtures/quickstart.afb.hex
	mkdir -p ../build
	python3 ../tests/make_fixtures.py ../build

$(TEST_BUILD)/test_aica: tests/test_aica.c $(TEST_BUILD)/libaicaflow_test.a
	$(CC) $(TEST_CFLAGS) -Iemu -I../sh4/include -I../include -I../format/include \
		tests/test_aica.c $(TEST_BUILD)/libaicaflow_test.a -lm -o $@

# Sources with #embed are expanded first; see tests/expand_embed.py.
$(TEST_BUILD)/src/%.c: tests/%.c tests/expand_embed.py $(FIXTURES) ../../firmware/aicaflow.drv
	$(EXPAND) $< $@
$(TEST_BUILD)/src/check_%.c: ../tests/check_%.c tests/expand_embed.py $(FIXTURES) ../../firmware/aicaflow.drv
	$(EXPAND) $< $@

$(TEST_BUILD)/test_host: $(TEST_BUILD)/src/test_host.c tests/host_clock.c tests/kos.h $(TEST_BUILD)/libaicaflow_test.a
	$(CC) $(TEST_CFLAGS) -Itests -Iemu -Iinclude -I../sh4/include -I../include -I../format/include \
		$< tests/host_clock.c $(TEST_BUILD)/libaicaflow_test.a -lm -o $@

$(TEST_BUILD)/check_%: $(TEST_BUILD)/src/check_%.c tests/host_clock.c tests/kos.h $(TEST_BUILD)/libaicaflow_test.a
	$(CC) $(TEST_CFLAGS) -Wno-unused-parameter -Itests -Iinclude -I../sh4/include -I../include -I../format/include \
		$< tests/host_clock.c $(TEST_BUILD)/libaicaflow_test.a -lm -o $@

check: $(TEST_BUILD)/test_aica $(TEST_BUILD)/test_host $(addprefix $(TEST_BUILD)/,$(CHECKS))
	$(TEST_BUILD)/test_aica
	$(TEST_BUILD)/test_host
	@for check in $(CHECKS); do echo "== $$check"; $(TEST_BUILD)/$$check || exit 1; done

# The same checks built with Emscripten, without threads, run under Node: the
# interrupt goes to the next library call instead of a worker thread.
WEB_TEST_BUILD := build/web-test
WEB_TEST_FLAGS := -std=gnu2x -O2 $(WARNINGS) -Wno-unused-parameter $(DEFS) -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1

$(WEB_TEST_BUILD)/%.js: $(TEST_BUILD)/src/%.c tests/host_clock.c tests/kos.h libaicaflow_web.a
	@mkdir -p $(WEB_TEST_BUILD)
	$(EMCC) $(WEB_TEST_FLAGS) -Itests -Iemu -Iinclude -I../sh4/include -I../include -I../format/include \
		$< tests/host_clock.c libaicaflow_web.a -o $@
$(WEB_TEST_BUILD)/check_%.js: $(TEST_BUILD)/src/check_%.c tests/host_clock.c tests/kos.h libaicaflow_web.a
	@mkdir -p $(WEB_TEST_BUILD)
	$(EMCC) $(WEB_TEST_FLAGS) -Itests -Iinclude -I../sh4/include -I../include -I../format/include \
		$< tests/host_clock.c libaicaflow_web.a -o $@

web-check: $(WEB_TEST_BUILD)/test_host.js $(addprefix $(WEB_TEST_BUILD)/,$(addsuffix .js,$(CHECKS)))
	node $(WEB_TEST_BUILD)/test_host.js
	@for check in $(CHECKS); do echo "== $$check (web)"; node $(WEB_TEST_BUILD)/$$check.js || exit 1; done

clean:
	rm -rf build libaicaflow_pc.a libaicaflow_web.a afx_render

.PRECIOUS: $(TEST_BUILD)/src/%.c $(TEST_BUILD)/src/check_%.c
.PHONY: all web check web-check clean
