#!/usr/bin/env bash
# Run headless smokes with AddressSanitizer or ThreadSanitizer (native Linux/macOS).
#
#   ./scripts/sanitize_smoke.sh asan
#   ./scripts/sanitize_smoke.sh tsan
#   RTX_TARGETS=256 ./scripts/sanitize_smoke.sh asan
#
# Uses separate out-asan/bin-asan or out-tsan/bin-tsan trees (see make.shcc).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SAN="${1:-asan}"
case "$SAN" in
    asan|address) TASK="@smoke_asan" ;;
    tsan|thread)  TASK="@smoke_tsan" ;;
    *)
        echo "usage: $0 asan|tsan" >&2
        exit 2
        ;;
esac

export RTX_TARGETS="${RTX_TARGETS:-256}"
if [[ "$(uname -s)" == Darwin ]]; then
    export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:print_legend=0}"
else
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:print_legend=0}"
fi
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}"

exec ./make.shcc "$TASK"
