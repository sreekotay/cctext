#!/usr/bin/env bash
# Write a mixed prose/code (or JSON / CSV) fixture under testdata/generated/ (gitignored).
#
#   ./testdata/gen_large.sh [lines] [outfile]      # default 100000 lines
#   ./testdata/gen_large.sh --bytes 8G [outfile]    # ~8 GiB mixed text (slow)
#   ./testdata/gen_large.sh --bytes 2G --json       # ~2 GiB JSON array (slow)
#   ./testdata/gen_large.sh --bytes 3M --csv        # ~3 MiB CSV (quoted NL stays one record)
#   ./testdata/gen_large.sh --bytes 256M out.txt
set -euo pipefail

bytes_target=0
bytes_label=""
lines=""
kind=text
out=""

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

usage() {
    sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --json)
            kind=json
            shift
            ;;
        --csv)
            kind=csv
            shift
            ;;
        --bytes)
            bytes_label="${2:?gen_large: --bytes needs a size}"
            bytes_target="$(parse_size "$bytes_label")"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "gen_large: unknown flag $1" >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

if [[ "$bytes_target" -gt 0 ]]; then
    if [[ -n "${1:-}" ]]; then
        out="$1"
    elif [[ "$kind" == json ]]; then
        out="testdata/generated/large_${bytes_label}.json"
    elif [[ "$kind" == csv ]]; then
        out="testdata/generated/large_${bytes_label}.csv"
    else
        out="testdata/generated/large_${bytes_label}.txt"
    fi
elif [[ "$kind" == json ]]; then
    echo "gen_large: --json needs --bytes (e.g. --bytes 2G --json)" >&2
    exit 2
elif [[ "$kind" == csv ]]; then
    echo "gen_large: --csv needs --bytes (e.g. --bytes 3M --csv)" >&2
    exit 2
else
    lines="${1:-100000}"
    out="${2:-testdata/generated/large.txt}"
fi

dir="$(dirname "$out")"
mkdir -p "$dir"

if [[ "$bytes_target" -gt 0 && "$kind" == csv ]]; then
    # Records end on an unquoted newline. Widths vary on purpose: short
    # id/qty, medium sku/city, note is usually tiny and sometimes a long
    # quoted field (comma, wrap, or embedded NL) so a screen-local col_w
    # vector actually changes as you scroll.
    awk -v want="$bytes_target" '
    BEGIN {
        hdr = "id,sku,name,qty,price,note,city\n"
        printf "%s", hdr
        total = length(hdr)
        b = 0
        while (1) {
            city = (b % 5 == 0) ? "Austin" : (b % 5 == 1) ? "Boston" : \
                   (b % 5 == 2) ? "Kyoto" : (b % 5 == 3) ? "Lima" : "Oslo"
            name = (b % 41 == 0) ? "" : sprintf("item_%d", b)
            if (b % 53 == 0)
                note = "\"says \"\"hi\"\", really\""
            else if (b % 29 == 0)
                note = sprintf("\"line 1 of %d\n  indented line 2\nshort.\"", b)
            else if (b % 17 == 0)
                note = "\"A wrapping note that stays one record but is wide enough that an eighty-column pane must clip this column or steal from city, row " b ".\""
            else if (b % 7 == 0)
                note = "\"has, comma\""
            else
                note = "ok"
            rec = sprintf("%d,SKU-%06d,%s,%d,%.2f,%s,%s\n", \
                b, b, name, (b % 20) + 1, (b % 100) + 0.25, note, city)
            n = length(rec)
            if (total + n > want) break
            printf "%s", rec
            total += n
            b++
        }
    }
    ' > "$out"
elif [[ "$bytes_target" -gt 0 && "$kind" == json ]]; then
    # Valid JSON array of objects. Stop before the next record would
    # exceed the budget so the closing ] stays well-formed (may be a
    # few hundred bytes under --bytes).
    awk -v want="$bytes_target" '
    BEGIN {
        total = 1
        b = 0
        first = 1
        printf "["
        while (1) {
            rec = sprintf("\n  {\"id\": %d, \"name\": \"item_%d\", \"ok\": %s, \"n\": -2.5, \"s\": \"a\\nb\"}", b, b, (b % 2) ? "true" : "false")
            extra = first ? 0 : 1
            n = length(rec) + extra
            if (total + n + 2 > want) break
            if (!first) printf ","
            printf "%s", rec
            total += n
            first = 0
            b++
        }
        printf "\n]\n"
    }
    ' > "$out"
elif [[ "$bytes_target" -gt 0 ]]; then
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
