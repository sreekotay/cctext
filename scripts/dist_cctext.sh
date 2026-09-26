#!/usr/bin/env bash
# Pack a runnable cctext tree: binary + grammars/ next to it.
# The tarball also includes cctext-ui (libui-ng; AppKit on macOS, GTK 3 on
# Linux) in the same folder so e / Ctrl-E works. NO_UI=1 packs cctext only.
#
#   ./scripts/dist_cctext.sh          # builds if needed, writes dist/cctext-<os>-<arch>.tar.gz
#
# The tarball unpacks to a folder you can run in place:
#   tar -xzf cctext-macos-arm64.tar.gz && ./cctext-macos-arm64/cctext file.txt
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -x bin/cctext ]]; then
    ./make.shcc @cctext
fi
if [[ ! -x bin/cctext ]]; then
    echo "dist_cctext: bin/cctext missing" >&2
    exit 1
fi

have_ui=0
if [[ "${NO_UI:-0}" == 0 ]]; then
    if [[ ! -x bin/cctext-ui ]]; then
        ./make.shcc @cctext_ui
    fi
    if [[ -x bin/cctext-ui ]]; then
        have_ui=1
    else
        echo "dist_cctext: bin/cctext-ui missing (NO_UI=1 to pack cctext only)" >&2
        exit 1
    fi
fi

os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
    Darwin) os=macos ;;
    Linux) os=linux ;;
    *) os="$(printf '%s' "$os" | tr '[:upper:]' '[:lower:]')" ;;
esac
case "$arch" in
    arm64|aarch64) arch=arm64 ;;
    x86_64|amd64) arch=x64 ;;
esac

name="cctext-${os}-${arch}"
stage="$ROOT/dist/$name"
rm -rf "$stage"
mkdir -p "$stage/grammars"
cp bin/cctext "$stage/cctext"
chmod +x "$stage/cctext"
if [[ "$have_ui" == 1 ]]; then
    cp bin/cctext-ui "$stage/cctext-ui"
    chmod +x "$stage/cctext-ui"
fi
cp testdata/grammars/*.tmLanguage.json "$stage/grammars/"
{
    echo "cctext — console frontend"
    if [[ "$have_ui" == 1 ]]; then
        echo "cctext-ui — libui-ng window (same folder; e / Ctrl-E swaps)"
        if [[ "$os" == linux ]]; then
            echo "  (needs GTK 3 at run time: apt install libgtk-3-0)"
        fi
    fi
    echo "https://github.com/sreekotay/cctext"
    echo
    echo "  ./cctext file.txt"
    echo "  ./cctext --wrap file.txt"
    if [[ "$have_ui" == 1 ]]; then
        echo "  ./cctext-ui file.txt"
    fi
    echo
    echo "Grammars load from ./grammars next to the binary."
    if command -v ccc >/dev/null 2>&1; then
        echo
        echo "Built with: $(ccc --version 2>/dev/null | head -1)"
    fi
} >"$stage/README.txt"

mkdir -p "$ROOT/dist"
tar -C "$ROOT/dist" -czf "$ROOT/dist/${name}.tar.gz" "$name"
echo "wrote dist/${name}.tar.gz"
ls -l "$ROOT/dist/${name}.tar.gz"
