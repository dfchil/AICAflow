SHELL := /bin/bash

KOS_ENV ?= /opt/toolchains/dc/kos/environ.sh
EXAMPLES := quickstart multiple_dsp_effects dynamic_sfx dsp_effects_player music_player
TOOLS := tuner/server
C_COMPILER := build/afx_compile
C_COMPILER_TEST := build/test_afx_compile
N64_CSEQ_TEST := build/test_afx_n64_cseq
N64_SFX_TEST := build/test_afx_n64_sfx
DEMO_ASSETS := build/afx_demo_assets
BANK_COMPILER := build/afx_bank
PROFILE_COMPILER := build/afx_profile
VGM_COMPILER := build/afx_vgm
N64_COMPILER := build/afx_n64

.PHONY: all examples tools check compiler firmware firmware-check clean

all: examples tools

compiler: $(C_COMPILER) $(DEMO_ASSETS) $(BANK_COMPILER) $(PROFILE_COMPILER) $(VGM_COMPILER) $(N64_COMPILER)

examples:
	@for example in $(EXAMPLES); do \
		source $(KOS_ENV) && $(MAKE) -C examples/$$example || exit $$?; \
	done

tools:
	@for tool in $(TOOLS); do \
		source $(KOS_ENV) && $(MAKE) -C tools/$$tool || exit $$?; \
	done

check: format-check $(C_COMPILER) $(C_COMPILER_TEST) $(N64_CSEQ_TEST) $(N64_SFX_TEST) $(N64_COMPILER) $(BANK_COMPILER) $(PROFILE_COMPILER) $(VGM_COMPILER)
	$(MAKE) -C driver smoke
	./$(C_COMPILER_TEST)
	./$(N64_CSEQ_TEST)
	./$(N64_SFX_TEST)
	python3 tools/test/test_afx_vgm.py
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 tools/research/make_fixture_midi.py "$$task_tmp/fixture.mid" && \
	./$(C_COMPILER) "$$task_tmp/fixture.mid" --zones tools/test/fixtures/c_fixture.zones \
	"$$task_tmp/fixture.afb" "$$task_tmp/fixture.afx" && driver/build/afx_validate "$$task_tmp/fixture.afx"
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 tools/research/make_fixture_midi.py "$$task_tmp/fixture.mid" && \
	./$(C_COMPILER) "$$task_tmp/fixture.mid" --zones tools/test/fixtures/c_fixture.zones \
	"$$task_tmp/one.afb" "$$task_tmp/one.afx" && \
	./$(C_COMPILER) "$$task_tmp/fixture.mid" --zones tools/test/fixtures/c_fixture.zones \
	"$$task_tmp/two.afb" "$$task_tmp/two.afx" && mkdir "$$task_tmp/controls" && \
	./$(BANK_COMPILER) --merge "$$task_tmp/music.afb" "$$task_tmp/controls" "$$task_tmp/one.afx" "$$task_tmp/two.afx" && \
	driver/build/afx_validate "$$task_tmp/controls/one.afx" && driver/build/afx_validate "$$task_tmp/controls/two.afx" && \
	python3 -c 'import struct,sys; bank,flow,source=(open(p,"rb").read() for p in sys.argv[1:]); assert struct.unpack_from("<2I",bank,8)==struct.unpack_from("<2I",flow,40); assert bank[32:]==source[32:]' "$$task_tmp/music.afb" "$$task_tmp/controls/one.afx" "$$task_tmp/one.afb"
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 tools/research/make_fixture_midi.py "$$task_tmp/fixture.mid" && \
	./$(C_COMPILER) "$$task_tmp/fixture.mid" --zones tools/test/fixtures/c_fixture.zones \
	"$$task_tmp/fixture.afb" "$$task_tmp/fixture.afx" && \
	./$(PROFILE_COMPILER) init "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afp" room 112 && \
	python3 -m json.tool "$$task_tmp/fixture.afp" >/dev/null && \
	./$(PROFILE_COMPILER) describe "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afp" | grep -qx 'room 112 256' && \
	python3 -c 'import json,sys; p=sys.argv[1]; x=json.load(open(p)); x["templates"]={"test":{"parameters":{"lfo":19024}}}; x["setup_templates"]={"0":"test"}; open(p,"w").write(json.dumps(x))' "$$task_tmp/fixture.afp" && \
	./$(PROFILE_COMPILER) apply "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afc" "$$task_tmp/fixture.afp" \
	"$$task_tmp/profiled.afx" "$$task_tmp/profiled.afc" && driver/build/afx_validate "$$task_tmp/profiled.afx" && \
	python3 -c 'import struct,sys; base,derived=(open(p,"rb").read() for p in sys.argv[1:]); h=struct.unpack_from("<20I",base); q=struct.unpack_from("<20I",derived); a=base[h[4]+h[6]:]; b=derived[q[4]+q[6]:]; assert a[0]==0x14 and b[0]==0x10; pitch,mix=struct.unpack_from("<HH",a,4); mask=struct.unpack_from("<I",b,4)[0]; vals=iter(struct.unpack_from("<"+"H"*(mask.bit_count()),b,8)); fields={i:next(vals) for i in range(18) if mask>>i&1}; assert fields[6]==pitch and fields[7]==19024 and fields[10]==mix' "$$task_tmp/fixture.afx" "$$task_tmp/profiled.afx"
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 tools/research/make_fixture_midi.py "$$task_tmp/fixture.mid" && \
	./$(C_COMPILER) "$$task_tmp/fixture.mid" --zones tools/test/fixtures/c_fixture.zones \
	"$$task_tmp/fixture.afb" "$$task_tmp/fixture.afx" && \
	./$(PROFILE_COMPILER) init "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afp" dry 0 && \
	PYTHONPATH=tools/research python3 -c 'import json,sys; from pathlib import Path; from afx_visualize import decode,NOTE,NOTE_PL; p=Path(sys.argv[1]); x=json.loads(p.read_text()); actions,*_=decode(Path(sys.argv[2])); tick,(_,channel,_,_,_)=next((tick,event) for tick,event in actions if event[0] in (NOTE,NOTE_PL)); x["lanes"]=[{"event":{"kind":"note","tick":tick,"ordinal":0,"channel":channel},"offset":0,"parameters":{"mix":234}},{"event":{"kind":"note","tick":tick,"ordinal":0,"channel":channel},"offset":1,"parameters":{"mix":123}}]; p.write_text(json.dumps(x))' "$$task_tmp/fixture.afp" "$$task_tmp/fixture.afx" && \
	./$(PROFILE_COMPILER) apply "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afc" "$$task_tmp/fixture.afp" \
	"$$task_tmp/lane.afx" "$$task_tmp/lane.afc" && driver/build/afx_validate "$$task_tmp/lane.afx" && \
	PYTHONPATH=tools/research python3 -c 'import sys; from pathlib import Path; from afx_visualize import decode,NOTE_PL,PATCH_LEVEL; actions,*_=decode(Path(sys.argv[1])); assert any(tick == 0 and event[0] == NOTE_PL and event[4][-1] == 234 for tick,event in actions); assert any(tick == 1 and event[0] == PATCH_LEVEL and event[4] == [123] for tick,event in actions)' "$$task_tmp/lane.afx" && \
	./$(PROFILE_COMPILER) export-lanes "$$task_tmp/lane.afx" "$$task_tmp/fixture.afx" "$$task_tmp/exported.afp" dry 0 && \
	./$(PROFILE_COMPILER) apply "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afc" "$$task_tmp/exported.afp" \
	"$$task_tmp/exported.afx" "$$task_tmp/exported.afc" && driver/build/afx_validate "$$task_tmp/exported.afx" && \
	PYTHONPATH=tools/research python3 -c 'import sys; from pathlib import Path; from afx_visualize import decode,PATCH,PATCH_LEVEL; actions,*_=decode(Path(sys.argv[1])); assert any(tick == 1 and event[0] in (PATCH,PATCH_LEVEL) and event[3] == 1 << 10 and event[4] == [123] for tick,event in actions)' "$$task_tmp/exported.afx"
	PYTHONPATH=tools/research:tools/tuner python3 tools/test/test_afx_adpcm.py
	PYTHONPATH=tools/research:tools/tuner python3 tools/test/test_afx_midi.py
	PYTHONPATH=tools/research:tools/tuner python3 tools/test/test_afx_sf2.py
	PYTHONPATH=tools/research:tools/tuner python3 tools/test/test_make_fixture_midi.py
	PYTHONPATH=tools/tuner python3 tools/test/test_afx_tuner_client.py
	PYTHONPATH=tools/tuner python3 tools/test/test_afx_tuner_example.py
	PYTHONPATH=tools/research python3 tools/test/test_afx_n64.py

$(C_COMPILER): tools/author/afx_compile_c.c tools/author/afx_compile_c.h tools/author/afx_compile_c_cli.c tools/author/afx_midi_c.c tools/author/afx_midi_c.h tools/author/afx_sample_c.c tools/author/afx_sample_c.h tools/author/afx_sf2_c.c tools/author/afx_sf2_c.h tools/author/afx_ya2beam.c driver/common/codec.c driver/include/aicaflow/codec.h driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O2 -Wall -Wextra -Werror -Iformat/include -Idriver/include tools/author/afx_compile_c.c tools/author/afx_midi_c.c tools/author/afx_sample_c.c tools/author/afx_sf2_c.c tools/author/afx_ya2beam.c tools/author/afx_compile_c_cli.c driver/common/codec.c -lm -o $@

$(C_COMPILER_TEST): tools/author/afx_compile_c.c tools/author/afx_compile_c.h tools/author/afx_midi_c.c tools/author/afx_midi_c.h tools/author/afx_sample_c.c tools/author/afx_sample_c.h tools/author/afx_ya2beam.c tools/test/test_afx_compile_c.c driver/common/codec.c driver/include/aicaflow/codec.h driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Iformat/include -Idriver/include -Itools/author tools/author/afx_compile_c.c tools/author/afx_midi_c.c tools/author/afx_sample_c.c tools/author/afx_ya2beam.c tools/test/test_afx_compile_c.c driver/common/codec.c -lm -o $@

$(N64_CSEQ_TEST): tools/author/afx_n64_cseq.c tools/author/afx_n64_cseq.h tools/author/afx_compile_c.h tools/test/test_afx_n64_cseq.c driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Iformat/include -Idriver/include -Itools/author tools/author/afx_n64_cseq.c tools/test/test_afx_n64_cseq.c -o $@

$(N64_SFX_TEST): tools/test/test_afx_n64_sfx.c $(N64_COMPILER)
	mkdir -p build
	clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Iformat/include -Idriver/include -Itools/author tools/test/test_afx_n64_sfx.c tools/author/afx_n64_cseq.c tools/author/afx_compile_c.c tools/author/afx_sample_c.c tools/author/afx_ya2beam.c driver/common/codec.c -lm -o $@

$(DEMO_ASSETS): tools/author/afx_demo_assets.c tools/author/afx_compile_c.c tools/author/afx_compile_c.h driver/common/codec.c driver/include/aicaflow/codec.h driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O2 -Wall -Wextra -Werror -Iformat/include -Idriver/include tools/author/afx_demo_assets.c tools/author/afx_compile_c.c driver/common/codec.c -lm -o $@

$(BANK_COMPILER): tools/author/afx_bank_c.c tools/author/afx_compile_c.c tools/author/afx_compile_c.h tools/author/afx_midi_c.c tools/author/afx_midi_c.h tools/author/afx_sample_c.c tools/author/afx_sample_c.h tools/author/afx_sf2_c.c tools/author/afx_sf2_c.h tools/author/afx_ya2beam.c driver/common/codec.c driver/include/aicaflow/codec.h driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O2 -Wall -Wextra -Werror -Iformat/include -Idriver/include tools/author/afx_bank_c.c tools/author/afx_compile_c.c tools/author/afx_midi_c.c tools/author/afx_sample_c.c tools/author/afx_sf2_c.c tools/author/afx_ya2beam.c driver/common/codec.c -lm -o $@

$(PROFILE_COMPILER): tools/author/afx_profile_c.c driver/common/codec.c driver/include/aicaflow/codec.h driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O2 -Wall -Wextra -Werror -Iformat/include -Idriver/include tools/author/afx_profile_c.c driver/common/codec.c -o $@

$(VGM_COMPILER): tools/author/afx_vgm.c tools/author/afx_compile_c.c tools/author/afx_compile_c.h tools/author/afx_sample_c.c tools/author/afx_sample_c.h tools/author/afx_ya2beam.c driver/common/codec.c driver/include/aicaflow/codec.h driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O2 -Wall -Wextra -Werror -Iformat/include -Idriver/include -Itools/author tools/author/afx_vgm.c tools/author/afx_compile_c.c tools/author/afx_sample_c.c tools/author/afx_ya2beam.c driver/common/codec.c -lm -lz -o $@

$(N64_COMPILER): tools/author/afx_n64.c tools/author/afx_n64_cseq.c tools/author/afx_n64_cseq.h tools/author/afx_compile_c.c tools/author/afx_compile_c.h tools/author/afx_sample_c.c tools/author/afx_sample_c.h tools/author/afx_ya2beam.c driver/common/codec.c driver/include/aicaflow/codec.h driver/include/aicaflow/protocol.h
	mkdir -p build
	clang -std=c11 -O2 -Wall -Wextra -Werror -Iformat/include -Idriver/include -Itools/author tools/author/afx_n64.c tools/author/afx_n64_cseq.c tools/author/afx_compile_c.c tools/author/afx_sample_c.c tools/author/afx_ya2beam.c driver/common/codec.c -lm -o $@

firmware:
	source $(KOS_ENV) && $(MAKE) -C driver/arm7

firmware-check: firmware
	@expected=$$(python3 -c 'import json; print(json.load(open("firmware/manifest.json"))["sha256"])'); \
	actual=$$(shasum -a 256 firmware/aicaflow.drv | awk '{print $$1}'); \
	test "$$actual" = "$$expected" || { echo "firmware/aicaflow.drv differs from its release manifest" >&2; exit 1; }

clean:
	@for example in $(EXAMPLES); do $(MAKE) -C examples/$$example clean; done
	@for tool in $(TOOLS); do $(MAKE) -C tools/$$tool clean; done
	$(MAKE) -C driver clean
	rm -f $(C_COMPILER) $(C_COMPILER_TEST) $(DEMO_ASSETS) $(BANK_COMPILER) $(PROFILE_COMPILER) \
		$(VGM_COMPILER) $(N64_COMPILER) $(N64_CSEQ_TEST) $(N64_SFX_TEST) build/afx_compile_c build/test_afx_compile_c build/afx_bank_c build/afx_profile_c

# The public format header is an independent dependency of every native tool.
$(C_COMPILER) $(C_COMPILER_TEST) $(N64_CSEQ_TEST) $(N64_SFX_TEST) $(DEMO_ASSETS) $(BANK_COMPILER) $(PROFILE_COMPILER) $(VGM_COMPILER) $(N64_COMPILER): format/include/aicaflow/format.h

format-check:
	$(MAKE) -C format check

.PHONY: format-check
