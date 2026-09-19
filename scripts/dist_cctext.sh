#!/usr/bin/env bash
# Pack a runnable cctext tree: binary + grammars/ next to it.
# On macOS the tarball also includes cctext-gui and cctext-ui
# (same folder so e / Ctrl-E works).
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

have_gui=0
have_ui=0
if [[ "$(uname -s)" == Darwin ]]; then
    if [[ ! -x bin/cctext-gui ]]; then
        ./make.shcc @cctext_gui
    fi
    if [[ -x bin/cctext-gui ]]; then
        have_gui=1
    else
        echo "dist_cctext: bin/cctext-gui missing (macOS dist includes it)" >&2
        exit 1
    fi
    if [[ ! -x bin/cctext-ui ]]; then
        ./make.shcc @cctext_ui
    fi
    if [[ -x bin/cctext-ui ]]; then
        have_ui=1
    else
        echo "dist_cctext: bin/cctext-ui missing (macOS dist includes it)" >&2
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
if [[ "$have_gui" == 1 ]]; then
    cp bin/cctext-gui "$stage/cctext-gui"
    chmod +x "$stage/cctext-gui"
fi
if [[ "$have_ui" == 1 ]]; then
    cp bin/cctext-ui "$stage/cctext-ui"
    chmod +x "$stage/cctext-ui"
fi
cp testdata/grammars/*.tmLanguage.json "$stage/grammars/"
{
    echo "cctext — console frontend"
    if [[ "$have_gui" == 1 ]]; then
        echo "cctext-gui — Cocoa + Core Text (same folder; e / Ctrl-E swaps)"
    fi
    if [[ "$have_ui" == 1 ]]; then
        echo "cctext-ui — libui-ng window, Core Text text (same folder)"
    fi
    echo "https://github.com/sreekotay/cctext"
    echo
    echo "  ./cctext file.txt"
    echo "  ./cctext --wrap file.txt"
    if [[ "$have_gui" == 1 ]]; then
        echo "  ./cctext-gui file.txt"
    fi
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
