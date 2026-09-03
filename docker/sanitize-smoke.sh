#!/usr/bin/env bash
# Build a sanitizer image and run headless @smoke under ASAN or TSAN.
#
#   ./docker/sanitize-smoke.sh asan
#   ./docker/sanitize-smoke.sh tsan
#   RTX_TARGETS=256 MEMORY=4g ./docker/sanitize-smoke.sh asan
#
# Env:
#   CPUS=2                docker --cpus (TSAN benefits from >1 core)
#   MEMORY=4g             docker --memory (sanitizer overhead; dup_scale @256 MiB)
#   CCC_REF=              pin concurrent-c git ref at build time
#   SKIP_BUILD=1          reuse existing image
#   RTX_TARGETS=256       dup_scale checkpoint (default in @smoke_asan/@smoke_tsan)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SAN="${1:-asan}"
case "$SAN" in
    asan|address)
        SANITIZER=address
        IMAGE="${IMAGE:-cctext-asan}"
        TASK="@smoke_asan"
        LOG="${LOG:-$ROOT/testdata/generated/docker_asan_smoke.log}"
        ;;
    tsan|thread)
        SANITIZER=thread
        IMAGE="${IMAGE:-cctext-tsan}"
        TASK="@smoke_tsan"
        LOG="${LOG:-$ROOT/testdata/generated/docker_tsan_smoke.log}"
        ;;
    *)
        echo "usage: $0 asan|tsan" >&2
        exit 2
        ;;
esac

CPUS="${CPUS:-2}"
MEMORY="${MEMORY:-4g}"
CCC_REF="${CCC_REF:-}"

mkdir -p "$(dirname "$LOG")"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "docker: building $IMAGE (SANITIZER=$SANITIZER)…" >&2
    build_args=(
        -f "$ROOT/docker/Dockerfile.sanitize"
        --build-arg "SANITIZER=$SANITIZER"
        -t "$IMAGE"
        "$ROOT"
    )
    if [[ -n "$CCC_REF" ]]; then
        build_args=(--build-arg "CCC_REF=$CCC_REF" "${build_args[@]}")
    fi
    docker build "${build_args[@]}"
fi

echo "docker: $TASK @ ${CPUS} CPU, ${MEMORY} RAM → $LOG" >&2
{
    echo "# cctext sanitizer docker smoke ($SAN)"
    echo "# date=$(date -u +%Y-%m-%dT%H:%M:%SZ)  sanitizer=$SANITIZER  cpus=$CPUS  memory=$MEMORY"
    echo "# image=$IMAGE  task=$TASK"
    echo
    docker run --rm \
        --cpus="$CPUS" \
        --memory="$MEMORY" \
        --memory-swap="$MEMORY" \
        --pids-limit=512 \
        -e RTX_TARGETS="${RTX_TARGETS:-256}" \
        -e RTX_DUP_ROUNDS="${RTX_DUP_ROUNDS:-}" \
        -e ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:print_legend=0}" \
        -e TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}" \
        "$IMAGE" \
        /usr/bin/time -f 'elapsed_sec=%e max_rss_kb=%M' \
        ./make.shcc "$TASK"
} 2>&1 | tee "$LOG"

echo "docker: log → $LOG" >&2
