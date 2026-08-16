#!/usr/bin/env bash
# Write a mixed prose/code fixture. Default 100000 lines (~4–6 MiB).
#   ./testdata/gen_large.sh [lines] [outfile]
set -euo pipefail

lines="${1:-100000}"
out="${2:-testdata/generated/large.txt}"
dir="$(dirname "$out")"
mkdir -p "$dir"

if ! [[ "$lines" =~ ^[0-9]+$ ]] || [ "$lines" -lt 16 ]; then
    echo "gen_large: lines must be an integer >= 16, got: $lines" >&2
    exit 1
fi

# 8-line block: 2 prose + 6 code. awk is much faster than a shell loop.
awk -v want="$lines" '
BEGIN {
    n = 0
    b = 0
    while (n < want) {
        if (n < want) { print "=== prose ==="; n++ }
        if (n < want) {
            printf "Block %d. A *bold* word and `mono` in a wrapping paragraph.\n", b
            n++
        }
        if (n < want) { print "=== code ==="; n++ }
        if (n < want) { printf "int fn_%d(void) {\n", b; n++ }
        if (n < want) { printf "    /* block %d */\n", b; n++ }
        if (n < want) { printf "    int x = %d;\n", b; n++ }
        if (n < want) { print "    return x;"; n++ }
        if (n < want) { print "}"; n++ }
        b++
    }
}
' > "$out"

bytes="$(wc -c < "$out" | tr -d ' ')"
got="$(wc -l < "$out" | tr -d ' ')"
echo "wrote $out  lines=$got  bytes=$bytes"
