# ccc from a concurrent-c checkout treats --out-dir as relative to that
# repo. Pass absolute paths so a standalone tree does not write there.
CCC ?= ccc
OUT := $(CURDIR)/out
BIN := $(CURDIR)/bin

# 0.3.3-139: passthrough CamelCase C types from #include <…>.
CCC_FLAGS := version=0.3.3-139 --no-cache --out-dir $(OUT) --bin-dir $(BIN)
RAYLIB_PREFIX := $(shell brew --prefix raylib 2>/dev/null)
RAYLIB_CFLAGS := -I$(RAYLIB_PREFIX)/include
RAYLIB_LIBS := -L$(RAYLIB_PREFIX)/lib -lraylib

.PHONY: setup test smoke cctext raytext clean

setup:
	./setup.sh

smoke test:
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run piece_tree_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run layout_measure_smoke

cctext:
	$(CCC) $(CCC_FLAGS) build --build-file build.cc cctext

raytext:
	$(CCC) $(CCC_FLAGS) --cc-flags "$(RAYLIB_CFLAGS)" --ld-flags "$(RAYLIB_LIBS)" \
		build --build-file build.cc raytext

clean:
	rm -rf $(OUT) $(BIN) testdata/generated
