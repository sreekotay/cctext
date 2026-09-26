#!/usr/bin/env bash
# Workbook scale pins (docs/workbook.md "W2 at scale"): wb_scale_perf --pins
# on a generated 100k-row workbook with 1000 aggregates; each op's p50
# against its own pin in testdata/perf/wb_pins.env (not a blanket floor:
# a 10 µs cell edit is pinned in tenths of a millisecond).
#
#   ./scripts/wb_perf_pins.sh           # run and compare (fail on regression)
#   ./scripts/wb_perf_pins.sh record    # rewrite the pins from this run
#
# Env:
#   RTX_PERF_FACTOR=3           fail if a p50 > pin * factor
#   RTX_WB_PIN_HEADROOM=2       record stores p50 * headroom ...
#   RTX_WB_PIN_FLOOR_MS=0.1     ... but at least this
#   RTX_WB_PIN_RSS_HEADROOM=1.25  open RSS pin (checked without the factor)
#   RTX_WB_PIN_ROWS=100000
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-check}"
case "$MODE" in
    check|record) ;;
    -h|--help)
        sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "usage: $0 [check|record]" >&2
        exit 2
        ;;
esac

CCC="${CCC:-ccc}"
PINS="${RTX_WB_PINS:-testdata/perf/wb_pins.env}"
FACTOR="${RTX_PERF_FACTOR:-3}"
HEADROOM="${RTX_WB_PIN_HEADROOM:-2}"
FLOOR="${RTX_WB_PIN_FLOOR_MS:-0.1}"
RSS_HEADROOM="${RTX_WB_PIN_RSS_HEADROOM:-1.25}"
ROWS="${RTX_WB_PIN_ROWS:-100000}"

echo "wb_perf_pins: building wb_scale_perf (release)" >&2
"$CCC" --release --out-dir out --bin-dir bin build --build-file build_tests.cc wb_scale_perf
echo "wb_perf_pins: wb_scale_perf --pins $ROWS" >&2
OUT="$(./bin/wb_scale_perf --pins "$ROWS")"
printf '%s\n' "$OUT"

declare -A MS=()
RSS=""
while read -r _ _ opkv _ p50kv rest; do
    op="${opkv#op=}"
    MS["$op"]="${p50kv#p50_ms=}"
    if [[ "$op" == open ]]; then
        for kv in $rest; do
            [[ "$kv" == rss_mb=* ]] && RSS="${kv#rss_mb=}"
        done
    fi
done < <(printf '%s\n' "$OUT" | grep '^RESULT ')

OPS="open edit_1 edit_100 edit_1000 row_ins_del param reparse key_cell key_cell_px key_formula page"

if [[ "$MODE" == record ]]; then
    {
        echo "# Workbook scale pins. scripts/wb_perf_pins.sh record"
        echo "# Measured $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(hostname -s 2>/dev/null || echo unknown):"
        echo "# wb_scale_perf --pins $ROWS (1000 aggregates). Timing pins = max(p50 * $HEADROOM, $FLOOR ms);"
        echo "# check fails if a p50 > pin * RTX_PERF_FACTOR (default $FACTOR). RSS pin = rss * $RSS_HEADROOM."
        echo "rows=$ROWS"
        for op in $OPS; do
            v="${MS[$op]:-}"
            [[ -n "$v" ]] || continue
            awk -v op="$op" -v x="$v" -v h="$HEADROOM" -v f="$FLOOR" 'BEGIN {
                p = x * h; if (p < f) p = f; p = int(p * 100 + 0.999) / 100
                printf "wb_%s_ms=%.2f\n", op, p }'
        done
        if [[ -n "$RSS" ]]; then
            awk -v x="$RSS" -v h="$RSS_HEADROOM" 'BEGIN { printf "wb_open_rss_mb=%d\n", int(x * h + 0.999) }'
        fi
    } >"$PINS"
    echo "wb_perf_pins: wrote $PINS" >&2
    cat "$PINS"
    exit 0
fi

[[ -f "$PINS" ]] || { echo "wb_perf_pins: no $PINS (run: $0 record)" >&2; exit 1; }
status=0
for op in $OPS; do
    pin="$(grep -E "^wb_${op}_ms=" "$PINS" | head -1 | cut -d= -f2 || true)"
    m="${MS[$op]:-}"
    [[ -n "$pin" && -n "$m" ]] || continue
    lim="$(awk -v p="$pin" -v f="$FACTOR" 'BEGIN { printf "%.3f", p * f }')"
    if awk -v m="$m" -v l="$lim" 'BEGIN { exit !(m > l) }'; then
        echo "REGRESS wb_$op: p50=${m}ms  pin=${pin}ms  limit=${lim}ms (${FACTOR}x)" >&2
        status=1
    else
        echo "ok      wb_$op: p50=${m}ms  pin=${pin}ms  limit=${lim}ms" >&2
    fi
done
pin="$(grep -E '^wb_open_rss_mb=' "$PINS" | head -1 | cut -d= -f2 || true)"
if [[ -n "$pin" && -n "$RSS" ]]; then
    if awk -v m="$RSS" -v p="$pin" 'BEGIN { exit !(m > p) }'; then
        echo "REGRESS wb_open_rss: ${RSS} MB  pin=${pin} MB" >&2
        status=1
    else
        echo "ok      wb_open_rss: ${RSS} MB  pin=${pin} MB" >&2
    fi
fi
exit "$status"
