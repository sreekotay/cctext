CCC ?= ccc
# Default --release: -O2 -DNDEBUG + dead-strip. `make DEBUG=1 …` keeps asserts (-g).
# Emit cache is on so unchanged TUs skip shadow_lower. Cold rebuild: CC_NO_CACHE=1.
ifeq ($(DEBUG),1)
CCC_FLAGS := -g --out-dir out --bin-dir bin
else
CCC_FLAGS := --release --out-dir out --bin-dir bin
endif
RAYLIB_PREFIX := $(shell brew --prefix raylib 2>/dev/null)
RAYLIB_CFLAGS := -I$(RAYLIB_PREFIX)/include
RAYLIB_LIBS := -L$(RAYLIB_PREFIX)/lib -lraylib -lobjc -framework Foundation

LARGE_BYTES ?= 3M
LARGE := testdata/generated/large.txt

.PHONY: setup test smoke large giant giant-json giant-smoke perf perf-check perf-record dup-scale-8g dup-scale-64g cctext cctext-ray dist-cctext clean

setup:
	./setup.sh

$(LARGE): testdata/gen_large.sh
	./testdata/gen_large.sh --bytes $(LARGE_BYTES) $(LARGE)

large: $(LARGE)

smoke test: $(LARGE)
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run piece_tree_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run layout_measure_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run edit_session_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run find_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run hex_view_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run line_index_prop_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run tm_grammar_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run tm_lookback_probe
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run large_file_smoke
	# ~1 GiB select-all paste; for bigger: make dup-scale-8g / dup-scale-64g
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run dup_scale_smoke

# Grow through checkpoints via append_copy (page-store spill). Not default smoke.
dup-scale-8g: $(LARGE)
	RTX_TARGETS=1000,8192 RTX_DUP_ROUNDS=24 $(CCC) $(CCC_FLAGS) \
		build --build-file build.cc run dup_scale_smoke

dup-scale-64g: $(LARGE)
	RTX_TARGETS=1000,8192,65536 RTX_DUP_ROUNDS=24 $(CCC) $(CCC_FLAGS) \
		build --build-file build.cc run dup_scale_smoke

# Multi-GiB on-disk fixture (gitignored). Slow write; not part of smoke.
#   make giant              # → testdata/generated/large_8G.txt
#   make giant-json         # → testdata/generated/large_2G.json
#   make giant-smoke       # open / mid-line / tiny insert
GIANT_BYTES ?= 8G
GIANT := testdata/generated/large_$(GIANT_BYTES).txt
giant: $(GIANT)
$(GIANT): testdata/gen_large.sh
	./testdata/gen_large.sh --bytes $(GIANT_BYTES) $(GIANT)

GIANT_JSON_BYTES ?= 2G
GIANT_JSON := testdata/generated/large_$(GIANT_JSON_BYTES).json
giant-json: $(GIANT_JSON)
$(GIANT_JSON): testdata/gen_large.sh
	./testdata/gen_large.sh --bytes $(GIANT_JSON_BYTES) --json $(GIANT_JSON)

giant-smoke: $(GIANT)
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run giant_open_smoke -- $(GIANT)

# Pin / watch timings (needs large.txt; 8G if present). See scripts/perf_baseline.sh.
perf: $(LARGE)
	./scripts/perf_baseline.sh
perf-check: $(GIANT)
	./scripts/perf_baseline.sh check
perf-record: $(LARGE)
	./scripts/perf_baseline.sh record

cctext:
	$(CCC) $(CCC_FLAGS) build --build-file build.cc cctext

# Binary + grammars/ → dist/cctext-<os>-<arch>.tar.gz (not committed).
dist-cctext: cctext
	./scripts/dist_cctext.sh

# ccc target is cctext_ray (identifier); the installed name is cctext-ray.
cctext-ray:
	$(CCC) $(CCC_FLAGS) --cc-flags "$(RAYLIB_CFLAGS)" --ld-flags "$(RAYLIB_LIBS)" \
		build --build-file build.cc cctext_ray
	mv -f bin/cctext_ray bin/cctext-ray

clean:
	rm -rf out bin dist testdata/generated
