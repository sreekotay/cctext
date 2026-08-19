#!/usr/bin/env bash
# Pack a runnable cctext tree: binary + grammars/ next to it.
#
#   ./scripts/dist_cctext.sh          # builds if needed, writes dist/cctext-<os>-<arch>.tar.gz
#
# The tarball unpacks to a folder you can run in place:
#   tar -xzf cctext-macos-arm64.tar.gz && ./cctext-macos-arm64/cctext file.txt
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -x bin/cctext ]]; then
    make cctext
fi
if [[ ! -x bin/cctext ]]; then
    echo "dist_cctext: bin/cctext missing" >&2
    exit 1
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
cp testdata/grammars/*.tmLanguage.json "$stage/grammars/"
{
    echo "cctext — console frontend of raytext"
    echo "https://github.com/sreekotay/raytext"
    echo
    echo "  ./cctext file.txt"
    echo "  ./cctext --wrap file.txt"
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
