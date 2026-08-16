#!/usr/bin/env bash
# Check the Concurrent-C toolchain and optional Raylib. Does not vendor either.
set -euo pipefail

CCC="${CCC:-ccc}"

if ! command -v "$CCC" >/dev/null 2>&1; then
    echo "raytext: '$CCC' not on PATH." >&2
    echo "  brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git" >&2
    echo "  brew install --HEAD sreekotay/concurrent-c/ccc" >&2
    echo "  # or: PREFIX=\$HOME/.local ./cc-install.sh from a concurrent-c checkout" >&2
    echo "  # or: CCC=/path/to/ccc $0" >&2
    exit 1
fi

ver="$("$CCC" --version 2>/dev/null || true)"
echo "ccc: $ver"

need_major=0
need_minor=3
# Pin in sources is version=0.3.3 — accept any 0.3.x prefix.
case "$ver" in
    *0.3.*) ;;
    *)
        echo "raytext: expected ccc 0.3.x (sources pin version=0.3.3), got: $ver" >&2
        exit 1
        ;;
esac

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists raylib; then
    echo "raylib: $(pkg-config --modversion raylib)"
elif command -v brew >/dev/null 2>&1 && brew --prefix raylib >/dev/null 2>&1; then
    echo "raylib: $(brew --prefix raylib)"
else
    echo "raylib: not found (optional until the GUI frontend). brew install raylib"
fi

mkdir -p testdata/generated out bin
echo "ok"
