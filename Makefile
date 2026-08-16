# ccc from a concurrent-c checkout treats --out-dir as relative to that
# repo. Pass absolute paths so a standalone tree does not write there.
CCC ?= ccc
OUT := $(CURDIR)/out
BIN := $(CURDIR)/bin

CCC_FLAGS := version=0.3.3 --no-cache --out-dir $(OUT) --bin-dir $(BIN)

.PHONY: setup test smoke cctext clean

setup:
	./setup.sh

smoke test:
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run piece_tree_smoke
	$(CCC) $(CCC_FLAGS) build --build-file build.cc run layout_measure_smoke

cctext:
	$(CCC) $(CCC_FLAGS) build --build-file build.cc cctext

clean:
	rm -rf $(OUT) $(BIN) testdata/generated
