#!/usr/bin/env bash
# Capture / check editor timings; write a dated results table.
#
#   ./scripts/perf_baseline.sh            # run matrix, write results, compare pins
#   ./scripts/perf_baseline.sh record     # also refresh testdata/perf/baseline.env pins
#   ./scripts/perf_baseline.sh check      # fail on regression vs baseline.env
#   ./scripts/perf_baseline.sh run        # matrix only (no compare)
#
# Results land in testdata/perf/results/baseline_results_YYYY_MM_DD.txt
#
# Env:
#   GIANT=path            8G fixture (default: testdata/generated/large_8G.txt)
#   LARGE=path            ~2 MiB fixture (default: testdata/generated/large.txt)
#   RTX_PERF_FACTOR=3     fail if measured > pin * factor
#   RTX_PERF_HEADROOM=2   record stores ceil(ms * headroom)
#   RTX_PERF_FLOOR_MS=25  min pin for ops
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-}"
case "$MODE" in
    ""|run|record|check) ;;
    -h|--help)
        sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "usage: $0 [run|record|check]" >&2
        exit 2
        ;;
esac

CCC="${CCC:-ccc}"
CCC_FLAGS=(--no-cache --out-dir out --bin-dir bin)
LARGE="${LARGE:-testdata/generated/large.txt}"
GIANT="${GIANT:-testdata/generated/large_8G.txt}"
BASELINE="${RTX_PERF_BASELINE:-testdata/perf/baseline.env}"
RESULTS_DIR="${RTX_PERF_RESULTS:-testdata/perf/results}"
FACTOR="${RTX_PERF_FACTOR:-3}"
HEADROOM="${RTX_PERF_HEADROOM:-2}"
FLOOR_MS="${RTX_PERF_FLOOR_MS:-25}"
LOG="testdata/generated/perf_runs.log"

stamp_day="$(date -u +%Y_%m_%d)"
stamp_iso="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
host="$(hostname -s 2>/dev/null || hostname || echo unknown)"
RESULTS="$RESULTS_DIR/baseline_results_${stamp_day}.txt"

mkdir -p testdata/perf "$RESULTS_DIR" testdata/generated

if [[ ! -f "$LARGE" ]]; then
    ./testdata/gen_large.sh 100000 "$LARGE"
fi

run_one() {
    local path="$1" label="$2"
    if [[ ! -f "$path" ]]; then
        echo "perf_baseline: skip $label (missing $path)" >&2
        return 0
    fi
    echo "perf_baseline: matrix $label ← $path" >&2
    "$CCC" "${CCC_FLAGS[@]}" build --build-file build.cc run perf_matrix_smoke -- "$path" "$label"
}

# Build once, then run each size.
echo "perf_baseline: building perf_matrix_smoke" >&2
"$CCC" "${CCC_FLAGS[@]}" build --build-file build.cc perf_matrix_smoke

{
    echo "# raytext perf results"
    echo "# date=$stamp_iso  host=$host"
    echo "#"
    echo "# size       op              ms"
    echo "# --------------------------------"
} >"$RESULTS"

ALL_OUT=""
for pair in "$LARGE:2M" "$GIANT:8G"; do
    path="${pair%%:*}"
    label="${pair##*:}"
    if [[ ! -f "$path" ]]; then
        echo "perf_baseline: skip $label (missing $path)" >&2
        continue
    fi
    out="$(run_one "$path" "$label" 2>&1)" || {
        printf '%s\n' "$out" >&2
        exit 1
    }
    printf '%s\n' "$out"
    ALL_OUT+="$out"$'\n'
    printf '%s\n' "$out" | grep '^RESULT ' | while read -r _ kv1 kv2 kv3; do
        size="${kv1#size=}"
        op="${kv2#op=}"
        ms="${kv3#ms=}"
        printf '%-10s %-14s %s\n' "$size" "$op" "$ms"
    done >>"$RESULTS"
done

{
    echo "#"
    echo "# pins (regression): $BASELINE"
} >>"$RESULTS"

echo "perf_baseline: wrote $RESULTS" >&2
cat "$RESULTS" >&2

echo "$stamp_iso host=$host" >>"$LOG"
printf '%s\n' "$ALL_OUT" | grep '^RESULT ' >>"$LOG" || true

# Pull 8G metrics for pin compare / record.
open_ms=""
screen_ms=""
jump100k_ms=""
insert_ms=""
newline_ms=""
while read -r _ kv1 kv2 kv3; do
    size="${kv1#size=}"
    op="${kv2#op=}"
    ms="${kv3#ms=}"
    [[ "$size" == "8G" ]] || continue
    case "$op" in
        open) open_ms="$ms" ;;
        scroll_40) screen_ms="$ms" ;;
        jump_100k) jump100k_ms="$ms" ;;
        insert_mid) insert_ms="$ms" ;;
        newline_mid) newline_ms="$ms" ;;
    esac
done < <(printf '%s\n' "$ALL_OUT" | grep '^RESULT ')

ceil_ms() {
    awk -v x="$1" -v h="$2" -v f="$3" 'BEGIN {
        if (x + 0 != x) exit 1
        v = x * h
        if (v != int(v)) v = int(v) + 1
        else v = int(v)
        if (f > 0 && v < f) v = f
        print v
    }'
}

write_baseline() {
    local o s j i n
    [[ -n "$open_ms" && -n "$screen_ms" && -n "$jump100k_ms" && -n "$insert_ms" ]] || {
        echo "perf_baseline: need 8G RESULT lines to record pins (make giant)" >&2
        exit 1
    }
    o="$(ceil_ms "$open_ms" "$HEADROOM" "$FLOOR_MS")"
    s="$(ceil_ms "$screen_ms" "$HEADROOM" "$FLOOR_MS")"
    j="$(ceil_ms "$jump100k_ms" "$HEADROOM" "$FLOOR_MS")"
    i="$(ceil_ms "$insert_ms" "$HEADROOM" "$FLOOR_MS")"
    n="$(ceil_ms "${newline_ms:-0}" "$HEADROOM" "$FLOOR_MS")"
    cat >"$BASELINE" <<EOF
# Giant-open perf pin (ms ceilings). scripts/perf_baseline.sh record
# Measured $stamp_iso on $host; pins = max(ceil(ms*$HEADROOM), ${FLOOR_MS}ms).
# check fails if measured > pin * RTX_PERF_FACTOR (default $FACTOR).
# Full table: $RESULTS
fixture=$GIANT
open_ms=$o
screen_ms=$s
jump100k_ms=$j
insert_ms=$i
newline_ms=$n
EOF
    echo "perf_baseline: wrote $BASELINE" >&2
    cat "$BASELINE"
}

load_baseline() {
    local k v
    b_open_ms=""
    b_screen_ms=""
    b_jump100k_ms=""
    b_insert_ms=""
    b_newline_ms=""
    while IFS='=' read -r k v; do
        case "$k" in
            open_ms) b_open_ms="$v" ;;
            screen_ms) b_screen_ms="$v" ;;
            jump100k_ms) b_jump100k_ms="$v" ;;
            insert_ms) b_insert_ms="$v" ;;
            newline_ms) b_newline_ms="$v" ;;
        esac
    done < <(grep -E '^(open|screen|jump100k|insert|newline)_ms=' "$BASELINE")
    if [[ -z "$b_open_ms" || -z "$b_screen_ms" || -z "$b_jump100k_ms" || -z "$b_insert_ms" ]]; then
        echo "perf_baseline: incomplete $BASELINE" >&2
        exit 1
    fi
    if [[ -z "$b_newline_ms" ]]; then b_newline_ms="$FLOOR_MS"; fi
}

compare() {
    local name measured pin limit status=0
    if [[ ! -f "$BASELINE" ]]; then
        echo "perf_baseline: no $BASELINE (run: $0 record)" >&2
        exit 1
    fi
    if [[ -z "$open_ms" ]]; then
        echo "perf_baseline: no 8G measurements to compare (make giant)" >&2
        exit 1
    fi
    load_baseline
    echo "perf_baseline: compare 8G vs $BASELINE (factor=$FACTOR)" >&2
    for name in open_ms screen_ms jump100k_ms insert_ms newline_ms; do
        measured="${!name:-}"
        if [[ -z "$measured" ]]; then continue; fi
        case "$name" in
            open_ms) pin="$b_open_ms" ;;
            screen_ms) pin="$b_screen_ms" ;;
            jump100k_ms) pin="$b_jump100k_ms" ;;
            insert_ms) pin="$b_insert_ms" ;;
            newline_ms) pin="$b_newline_ms" ;;
        esac
        limit="$(awk -v p="$pin" -v f="$FACTOR" 'BEGIN { printf "%.3f", p * f }')"
        if awk -v m="$measured" -v lim="$limit" 'BEGIN { exit !(m > lim) }'; then
            echo "REGRESS $name: measured=${measured}ms  pin=${pin}ms  limit=${limit}ms (${FACTOR}x)" >&2
            status=1
        else
            echo "ok      $name: measured=${measured}ms  pin=${pin}ms  limit=${limit}ms" >&2
        fi
    done
    return "$status"
}

case "$MODE" in
    run)
        ;;
    record)
        write_baseline
        ;;
    check|"")
        if [[ -f "$BASELINE" ]]; then
            compare
        else
            echo "perf_baseline: no baseline yet; run: $0 record" >&2
        fi
        ;;
esac

echo "perf_baseline: results → $RESULTS" >&2
