CC := clang
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
smoke: build/test_protocol build/test_bank build/test_dsp build/afx_validate
	./build/test_protocol
	./build/test_bank
	./build/test_dsp

check: smoke
	@set -e; cd ..; for test in $(notdir $(sort $(wildcard ../tools/test/test_*.py))); do PYTHONPATH=tools/research:tools/tuner python3 tools/test/$$test; done

build/test_protocol: tests/test_protocol.c common/codec.c include/aicaflow/codec.h include/aicaflow/protocol.h
	mkdir -p build
	$(CC) $(CFLAGS) -I../format/include -Iinclude tests/test_protocol.c common/codec.c -o $@

build/test_bank: tests/test_bank.c tests/test_transport.c sh4/src/allocator.c sh4/src/dma.c sh4/src/flow.c sh4/src/instance.c sh4/src/ipc.c sh4/src/dsp.c sh4/src/dsp_prefabs.c sh4/src/dsp_scene.c sh4/src/bank.c common/codec.c sh4/src/host_internal.h sh4/include/aicaflow/host.h sh4/include/aicaflow/bank.h sh4/include/aicaflow/dsp.h include/aicaflow/codec.h include/aicaflow/protocol.h tests/stubs/dc/spu.h tests/stubs/dc/g2bus.h tests/stubs/kos/timer.h tests/stubs/kos/cache.h
	mkdir -p build
	$(CC) $(CFLAGS) -D_GNU_SOURCE -pthread -Itests/stubs -Ish4/include -I../format/include -Iinclude tests/test_bank.c sh4/src/allocator.c sh4/src/dma.c sh4/src/flow.c sh4/src/instance.c sh4/src/ipc.c sh4/src/dsp.c sh4/src/dsp_prefabs.c sh4/src/dsp_scene.c sh4/src/bank.c common/codec.c -lm -o $@

build/test_dsp: tests/test_dsp.c sh4/src/dsp.c sh4/src/dsp_prefabs.c sh4/include/aicaflow/dsp.h include/aicaflow/protocol.h
	mkdir -p build
	$(CC) $(CFLAGS) -Ish4/include -I../format/include -Iinclude tests/test_dsp.c sh4/src/dsp.c sh4/src/dsp_prefabs.c -lm -o $@

build/afx_validate: ../tools/author/afx_validate.c common/codec.c include/aicaflow/codec.h include/aicaflow/protocol.h
	mkdir -p build
	$(CC) $(CFLAGS) -I../format/include -Iinclude ../tools/author/afx_validate.c common/codec.c -o $@

layout: build/test_protocol
	$(MAKE) -C arm7
	./build/test_protocol ../firmware/aicaflow.drv

timer-check:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -I../format/include -Iinclude -Ish4/include tests/check_timer.c sh4/libaicaflow_host.a -o build/check_timer.elf
	mkdcdisc -e build/check_timer.elf -o build/check_timer.cdi -n afx_timer -N

upload-bench:
	$(MAKE) -C arm7
	$(MAKE) -C sh4
	mkdir -p build
	kos-cc -std=gnu23 -O2 -Wall -Wextra -Werror -I../format/include -Iinclude -Ish4/include tests/bench_upload.c sh4/libaicaflow_host.a -o build/bench_upload.elf

.PHONY: smoke check layout timer-check upload-bench

build/test_protocol build/test_bank build/test_dsp build/afx_validate: ../format/include/aicaflow/format.h
