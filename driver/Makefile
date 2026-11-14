# Clang is the default; CC from the environment or command line selects another compiler.
ifeq ($(origin CC),default)
CC := clang
endif
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
smoke: build/test_protocol build/test_bank build/test_bank_trusted build/test_dsp build/afx_validate
	./build/test_protocol ../firmware/aicaflow.drv
	./build/test_bank
	./build/test_bank_trusted
	./build/test_dsp

check: smoke
	python3 tests/test_arm7.py
	cd .. && python3 tools/test/test_dynamic_sfx.py
	cd .. && python3 tools/test/test_afx_tuner_properties.py
	cd .. && PYTHONPATH=tools/tuner python3 tools/test/test_afx_tuner_client.py
	cd .. && PYTHONPATH=tools/tuner python3 tools/test/test_afx_tuner_example.py

build/test_protocol: tests/test_protocol.c arm7/sample_address.h common/firmware.c format/src/codec.c format/include/aicaflow/codec.h include/aicaflow/protocol.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iformat/include -Iinclude tests/test_protocol.c common/firmware.c format/src/codec.c -o $@

build/test_bank: VALIDATE_ASSETS = 1
build/test_bank_trusted: VALIDATE_ASSETS = 0
build/test_bank build/test_bank_trusted: tests/test_bank.c tests/test_transport.c sh4/src/allocator.c sh4/src/dma.c sh4/src/flow.c sh4/src/instance.c sh4/src/ipc.c sh4/src/dsp.c sh4/src/dsp_prefabs.c sh4/src/dsp_scene.c sh4/src/scene.c sh4/src/bank.c common/firmware.c format/src/codec.c sh4/src/host_internal.h sh4/include/aicaflow/host.h sh4/include/aicaflow/bank.h sh4/include/aicaflow/scene.h sh4/include/aicaflow/dsp.h format/include/aicaflow/codec.h include/aicaflow/protocol.h tests/stubs/dc/spu.h tests/stubs/dc/g2bus.h tests/stubs/dc/asic.h tests/stubs/kos/timer.h tests/stubs/kos/cache.h
	mkdir -p build
	$(CC) $(CFLAGS) -DAFX_VALIDATE_ASSETS=$(VALIDATE_ASSETS) -D_GNU_SOURCE -pthread -Itests/stubs -Ish4/include -Iformat/include -Iinclude tests/test_bank.c sh4/src/allocator.c sh4/src/dma.c sh4/src/flow.c sh4/src/instance.c sh4/src/ipc.c sh4/src/dsp.c sh4/src/dsp_prefabs.c sh4/src/dsp_scene.c sh4/src/scene.c sh4/src/bank.c common/firmware.c format/src/codec.c -lm -o $@

build/test_dsp: tests/test_dsp.c sh4/src/dsp.c sh4/src/dsp_prefabs.c sh4/include/aicaflow/dsp.h include/aicaflow/protocol.h
	mkdir -p build
	$(CC) $(CFLAGS) -Ish4/include -Iformat/include -Iinclude tests/test_dsp.c sh4/src/dsp.c sh4/src/dsp_prefabs.c -lm -o $@

build/afx_validate: tools/afx_validate.c format/src/codec.c format/include/aicaflow/codec.h include/aicaflow/protocol.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iformat/include -Iinclude tools/afx_validate.c format/src/codec.c -o $@

layout: build/test_protocol
	$(MAKE) -C arm7
	./build/test_protocol ../firmware/aicaflow.drv

timer-check:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -Iformat/include -Iinclude -Ish4/include tests/check_timer.c sh4/libaicaflow_host.a -o build/check_timer.elf
	mkdcdisc -e build/check_timer.elf -o build/check_timer.cdi -n afx_timer -N

# Songs on the console (tests/check_song.c): loops at the section's length
# for a continue join and a new-play join, a restart while stopping, pause and
# resume, and every runtime block given back.
song-check:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	python3 tests/make_fixtures.py build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -Iformat/include -Iinclude -Ish4/include tests/check_song.c sh4/libaicaflow_host.a -o build/check_song.elf

# Command semantics on the console (tests/check_commands.c): STOP behind
# ACTIVATE, channels back at STOP, replace, release when idle, held PATCH.
commands-check:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	python3 tests/make_fixtures.py build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -Iformat/include -Iinclude -Ish4/include tests/check_commands.c sh4/libaicaflow_host.a -o build/check_commands.elf

# Flows that play from several banks on the console (tests/check_banks.c):
# a setup in bank slot 1 sounds from that bank; the banks go with the flow.
banks-check:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	python3 tests/make_fixtures.py build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -Iformat/include -Iinclude -Ish4/include tests/check_banks.c sh4/libaicaflow_host.a -o build/check_banks.elf

# Scenes on the console (tests/check_scenes.c): a level scene and its slot
# loaded, played and released again and again beside a permanent scene.
scenes-check:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	python3 tests/make_fixtures.py build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -Iformat/include -Iinclude -Ish4/include tests/check_scenes.c sh4/libaicaflow_host.a -o build/check_scenes.elf

upload-bench:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -Iformat/include -Iinclude -Ish4/include tests/bench_upload.c sh4/libaicaflow_host.a -o build/bench_upload.elf

.PHONY: smoke check layout timer-check song-check commands-check banks-check scenes-check upload-bench

build/test_protocol build/test_bank build/test_bank_trusted build/test_dsp build/afx_validate: $(wildcard format/include/aicaflow/*.h) include/aicaflow/firmware.h
