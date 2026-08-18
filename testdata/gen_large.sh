#!/usr/bin/env bash
# Write a mixed prose/code fixture under testdata/generated/ (gitignored).
#
#   ./testdata/gen_large.sh [lines] [outfile]     # default 100000 lines
#   ./testdata/gen_large.sh --bytes 8G [outfile]   # ~8 GiB on disk (slow)
#   ./testdata/gen_large.sh --bytes 256M out.txt
set -euo pipefail

bytes_target=0
lines=""
out="testdata/generated/large.txt"

parse_size() {
    local s="$1"
    local n unit
    if [[ "$s" =~ ^([0-9]+)([KkMmGg]?)$ ]]; then
        n="${BASH_REMATCH[1]}"
        unit="${BASH_REMATCH[2]}"
    else
        echo "gen_large: bad size '$s' (e.g. 8G, 256M, 1048576)" >&2
        exit 1
    fi
    case "$unit" in
        [Kk]) echo $((n * 1024)) ;;
        [Mm]) echo $((n * 1024 * 1024)) ;;
        [Gg]) echo $((n * 1024 * 1024 * 1024)) ;;
        "") echo "$n" ;;
    esac
}

if [[ "${1:-}" == "--bytes" ]]; then
    bytes_target="$(parse_size "${2:?gen_large: --bytes needs a size}")"
    out="${3:-testdata/generated/large_${2}.txt}"
elif [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
else
    lines="${1:-100000}"
    out="${2:-testdata/generated/large.txt}"
fi

dir="$(dirname "$out")"
mkdir -p "$dir"

if [[ "$bytes_target" -gt 0 ]]; then
    # Stream fixed 8-line blocks until byte budget. No line-count cap.
    awk -v want="$bytes_target" '
    BEGIN {
        b = 0
        total = 0
        while (total < want) {
            line = sprintf("=== prose ===\nBlock %d. A *bold* word and `mono` in a wrapping paragraph that keeps going so an eighty-column pane actually splits this line into more than one visual row.\n=== code ===\nint fn_%d(void) {\n    /* block %d */\n    int x = %d;\n    return x;\n}\n", b, b, b, b)
            n = length(line)
            if (total + n > want) {
                n = want - total
                printf "%s", substr(line, 1, n)
                total += n
                break
            }
            printf "%s", line
            total += n
            b++
        }
    }
    ' > "$out"
else
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
                printf "Block %d. A *bold* word and `mono` in a wrapping paragraph that keeps going so an eighty-column pane actually splits this line into more than one visual row.\n", b
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
fi

bytes="$(wc -c < "$out" | tr -d ' ')"
got="$(wc -l < "$out" | tr -d ' ')"
echo "wrote $out  lines=$got  bytes=$bytes"
