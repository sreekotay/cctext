# ccc from a concurrent-c checkout treats --out-dir as relative to that
# repo. Pass absolute paths so a standalone tree does not write there.
CCC ?= ccc
OUT := $(CURDIR)/out
BIN := $(CURDIR)/bin

CCC_FLAGS := --no-cache --out-dir $(OUT) --bin-dir $(BIN)
RAYLIB_PREFIX := $(shell brew --prefix raylib 2>/dev/null)
RAYLIB_CFLAGS := -I$(RAYLIB_PREFIX)/include
RAYLIB_LIBS := -L$(RAYLIB_PREFIX)/lib -lraylib

LARGE_LINES ?= 100000
LARGE := testdata/generated/large.txt

.PHONY: setup test smoke large cctext raytext clean

setup:
	./setup.sh

$(LARGE): testdata/gen_large.sh
	./testdata/gen_large.sh $(LARGE_LINES) $(LARGE)

large: $(LARGE)

smoke test: $(LARGE)
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run piece_tree_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run layout_measure_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run edit_session_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run large_file_smoke

cctext:
	$(CCC) $(CCC_FLAGS) build --build-file build.cc cctext

raytext:
	$(CCC) $(CCC_FLAGS) --cc-flags "$(RAYLIB_CFLAGS)" --ld-flags "$(RAYLIB_LIBS)" \
		build --build-file build.cc raytext

clean:
	rm -rf $(OUT) $(BIN) testdata/generated
