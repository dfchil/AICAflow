# Shared CLI regression checks; VALIDATOR adds runtime integration when supplied.
BIN ?= build
TEST ?= test
RESEARCH ?= research
VALIDATOR ?= true
.PHONY: check
check:
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 $(TEST)/make_cli_fixture.py "$$task_tmp" && \
	$(BIN)/afx_compile "$$task_tmp/fixture.mid" --zones "$$task_tmp/fixture.zones" \
	"$$task_tmp/fixture.afb" "$$task_tmp/fixture.afx" && $(VALIDATOR) "$$task_tmp/fixture.afx"
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 $(TEST)/make_cli_fixture.py "$$task_tmp" && \
	$(BIN)/afx_compile "$$task_tmp/fixture.mid" --zones "$$task_tmp/fixture.zones" \
	"$$task_tmp/one.afb" "$$task_tmp/one.afx" && \
	$(BIN)/afx_compile "$$task_tmp/fixture.mid" --zones "$$task_tmp/fixture.zones" \
	"$$task_tmp/two.afb" "$$task_tmp/two.afx" && \
	cmp "$$task_tmp/one.afb" "$$task_tmp/two.afb" && cmp "$$task_tmp/one.afx" "$$task_tmp/two.afx" && \
	cmp "$$task_tmp/one.afc" "$$task_tmp/two.afc" && cmp "$$task_tmp/one.afv" "$$task_tmp/two.afv" && \
	mkdir "$$task_tmp/controls" && \
	$(BIN)/afx_bank --merge "$$task_tmp/music.afb" "$$task_tmp/controls" "$$task_tmp/one.afx" "$$task_tmp/two.afx" && \
	$(VALIDATOR) "$$task_tmp/controls/one.afx" && $(VALIDATOR) "$$task_tmp/controls/two.afx" && \
	python3 -c 'import struct,sys; bank,flow,source=(open(p,"rb").read() for p in sys.argv[1:]); assert struct.unpack_from("<2I",bank,8)==struct.unpack_from("<2I",flow,40); assert bank[32:]==source[32:]' "$$task_tmp/music.afb" "$$task_tmp/controls/one.afx" "$$task_tmp/one.afb"
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 $(TEST)/make_cli_fixture.py "$$task_tmp" && \
	$(BIN)/afx_compile "$$task_tmp/fixture.mid" --zones "$$task_tmp/fixture.zones" \
	"$$task_tmp/fixture.afb" "$$task_tmp/fixture.afx" && \
	$(BIN)/afx_profile init "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afp" room 112 && \
	python3 -m json.tool "$$task_tmp/fixture.afp" >/dev/null && \
	$(BIN)/afx_profile describe "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afp" | grep -qx 'room 112 256' && \
	python3 -c 'import json,sys; p=sys.argv[1]; x=json.load(open(p)); x["templates"]={"test":{"parameters":{"lfo":19024}}}; x["setup_templates"]={"0":"test"}; open(p,"w").write(json.dumps(x))' "$$task_tmp/fixture.afp" && \
	$(BIN)/afx_profile apply "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afc" "$$task_tmp/fixture.afp" \
	"$$task_tmp/profiled.afx" "$$task_tmp/profiled.afc" && $(VALIDATOR) "$$task_tmp/profiled.afx" && \
	python3 -c 'import struct,sys; base,derived=(open(p,"rb").read() for p in sys.argv[1:]); h=struct.unpack_from("<20I",base); q=struct.unpack_from("<20I",derived); a=base[h[4]+h[6]:]; b=derived[q[4]+q[6]:]; assert a[0]==0x14 and b[0]==0x10; pitch,mix=struct.unpack_from("<HH",a,4); mask=struct.unpack_from("<I",b,4)[0]; vals=iter(struct.unpack_from("<"+"H"*(mask.bit_count()),b,8)); fields={i:next(vals) for i in range(18) if mask>>i&1}; assert fields[6]==pitch and fields[7]==19024 and fields[10]==mix' "$$task_tmp/fixture.afx" "$$task_tmp/profiled.afx"
	@task_tmp=$$(mktemp -d); trap 'rm -rf "$$task_tmp"' EXIT; \
	python3 $(TEST)/make_cli_fixture.py "$$task_tmp" && \
	$(BIN)/afx_compile "$$task_tmp/fixture.mid" --zones "$$task_tmp/fixture.zones" \
	"$$task_tmp/fixture.afb" "$$task_tmp/fixture.afx" && \
	$(BIN)/afx_profile init "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afp" dry 0 && \
	PYTHONPATH=$(RESEARCH) python3 -c 'import json,sys; from pathlib import Path; from afx_visualize import decode,NOTE,NOTE_PL; p=Path(sys.argv[1]); x=json.loads(p.read_text()); actions,*_=decode(Path(sys.argv[2])); tick,(_,channel,_,_,_)=next((tick,event) for tick,event in actions if event[0] in (NOTE,NOTE_PL)); x["lanes"]=[{"event":{"kind":"note","tick":tick,"ordinal":0,"channel":channel},"offset":0,"parameters":{"mix":234}},{"event":{"kind":"note","tick":tick,"ordinal":0,"channel":channel},"offset":1,"parameters":{"mix":123}}]; p.write_text(json.dumps(x))' "$$task_tmp/fixture.afp" "$$task_tmp/fixture.afx" && \
	$(BIN)/afx_profile apply "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afc" "$$task_tmp/fixture.afp" \
	"$$task_tmp/lane.afx" "$$task_tmp/lane.afc" && $(VALIDATOR) "$$task_tmp/lane.afx" && \
	PYTHONPATH=$(RESEARCH) python3 -c 'import sys; from pathlib import Path; from afx_visualize import decode,NOTE_PL,PATCH_LEVEL; actions,*_=decode(Path(sys.argv[1])); assert any(tick == 0 and event[0] == NOTE_PL and event[4][-1] == 234 for tick,event in actions); assert any(tick == 1 and event[0] == PATCH_LEVEL and event[4] == [123] for tick,event in actions)' "$$task_tmp/lane.afx" && \
	$(BIN)/afx_profile export-lanes "$$task_tmp/lane.afx" "$$task_tmp/fixture.afx" "$$task_tmp/exported.afp" dry 0 && \
	$(BIN)/afx_profile apply "$$task_tmp/fixture.afx" "$$task_tmp/fixture.afc" "$$task_tmp/exported.afp" \
	"$$task_tmp/exported.afx" "$$task_tmp/exported.afc" && $(VALIDATOR) "$$task_tmp/exported.afx" && \
	PYTHONPATH=$(RESEARCH) python3 -c 'import sys; from pathlib import Path; from afx_visualize import decode,PATCH,PATCH_LEVEL; actions,*_=decode(Path(sys.argv[1])); assert any(tick == 1 and event[0] in (PATCH,PATCH_LEVEL) and event[3] == 1 << 10 and event[4] == [123] for tick,event in actions)' "$$task_tmp/exported.afx"
