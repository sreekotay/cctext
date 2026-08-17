CCC ?= ccc
CCC_FLAGS := --no-cache --out-dir out --bin-dir bin
RAYLIB_PREFIX := $(shell brew --prefix raylib 2>/dev/null)
RAYLIB_CFLAGS := -I$(RAYLIB_PREFIX)/include
RAYLIB_LIBS := -L$(RAYLIB_PREFIX)/lib -lraylib -lobjc -framework Foundation

LARGE_LINES ?= 100000
LARGE := testdata/generated/large.txt

.PHONY: setup test smoke large giant cctext raytext clean

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
	# 1 GiB select-all paste doubling — set RTX_TARGET_MIB to shrink for a quick loop
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run dup_scale_smoke

# Optional multi-GiB on-disk fixture (gitignored). Slow; not part of smoke.
#   make giant GIANT_BYTES=8G
GIANT_BYTES ?= 8G
GIANT := testdata/generated/large_$(GIANT_BYTES).txt
giant:
	./testdata/gen_large.sh --bytes $(GIANT_BYTES) $(GIANT)

cctext:
	$(CCC) $(CCC_FLAGS) build --build-file build.cc cctext

raytext:
	$(CCC) $(CCC_FLAGS) --cc-flags "$(RAYLIB_CFLAGS)" --ld-flags "$(RAYLIB_LIBS)" \
		build --build-file build.cc raytext

clean:
	rm -rf out bin testdata/generated
