#!/usr/bin/env bash
# Check the Concurrent-C toolchain. Does not vendor ccc.
set -euo pipefail

CCC="${CCC:-ccc}"

if ! command -v "$CCC" >/dev/null 2>&1; then
    echo "cctext: '$CCC' not on PATH." >&2
    echo "  brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git" >&2
    echo "  brew install --HEAD sreekotay/concurrent-c/ccc" >&2
    echo "  # or: PREFIX=\$HOME/.local ./cc-install.sh from a concurrent-c checkout" >&2
    echo "  # or: CCC=/path/to/ccc $0" >&2
    exit 1
fi

ver="$("$CCC" --version 2>/dev/null || true)"
echo "ccc: $ver"

case "$ver" in
    *0.3.*) ;;
    *)
        echo "cctext: expected ccc 0.3.x, got: $ver" >&2
        exit 1
        ;;
esac

mkdir -p testdata/generated out bin
if test ! -f testdata/generated/large.txt; then
    ./testdata/gen_large.sh --bytes 3M testdata/generated/large.txt
fi
echo "ok"
