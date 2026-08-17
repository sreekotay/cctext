CCC ?= ccc
CCC_FLAGS := --no-cache --out-dir out --bin-dir bin
RAYLIB_PREFIX := $(shell brew --prefix raylib 2>/dev/null)
RAYLIB_CFLAGS := -I$(RAYLIB_PREFIX)/include
RAYLIB_LIBS := -L$(RAYLIB_PREFIX)/lib -lraylib -lobjc -framework Foundation

LARGE_LINES ?= 100000
LARGE := testdata/generated/large.txt

.PHONY: setup test smoke large giant giant-smoke perf perf-check perf-record dup-scale-8g dup-scale-64g cctext raytext clean

setup:
	./setup.sh

$(LARGE): testdata/gen_large.sh
	./testdata/gen_large.sh $(LARGE_LINES) $(LARGE)

large: $(LARGE)

smoke test: $(LARGE)
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run piece_tree_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run layout_measure_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run edit_session_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run find_smoke
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
#   make giant-smoke       # open / mid-line / tiny insert
GIANT_BYTES ?= 8G
GIANT := testdata/generated/large_$(GIANT_BYTES).txt
giant: $(GIANT)
$(GIANT): testdata/gen_large.sh
	./testdata/gen_large.sh --bytes $(GIANT_BYTES) $(GIANT)

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

raytext:
	$(CCC) $(CCC_FLAGS) --cc-flags "$(RAYLIB_CFLAGS)" --ld-flags "$(RAYLIB_LIBS)" \
		build --build-file build.cc raytext

clean:
	rm -rf out bin testdata/generated
