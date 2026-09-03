#!/usr/bin/env bash
# Build the low-end image and run @smoke under single-core / tight RAM limits.
#
#   ./docker/run-smoke.sh
#   CPUS=1 CPUSET=0 MEMORY=1536m RTX_TARGETS=256 ./docker/run-smoke.sh
#
# Env:
#   IMAGE=cctext-lowend   docker tag
#   CPUSET=0              docker --cpuset-cpus (one real core; --cpus is quota)
#   CPUS=1                docker --cpus (CFS quota; not enough alone)
#   MEMORY=2g             docker --memory (dup_scale @smoke needs ~1 GiB doc + headroom)
#   CCC_REF=              pin concurrent-c git ref at build time
#   SKIP_BUILD=1          reuse existing image
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${IMAGE:-cctext-lowend}"
CPUSET="${CPUSET:-0}"
CPUS="${CPUS:-1}"
MEMORY="${MEMORY:-2g}"
CCC_REF="${CCC_REF:-}"
LOG="${LOG:-$ROOT/testdata/generated/docker_smoke.log}"

mkdir -p "$(dirname "$LOG")"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "docker: building $IMAGE (ubuntu + ccc; smoke runs at container start)…" >&2
    build_args=(-f "$ROOT/docker/Dockerfile" -t "$IMAGE" "$ROOT")
    if [[ -n "$CCC_REF" ]]; then
        build_args=(--build-arg "CCC_REF=$CCC_REF" "${build_args[@]}")
    fi
    docker build "${build_args[@]}"
fi

echo "docker: smoke @ ${CPUS} CPU, ${MEMORY} RAM → $LOG" >&2
{
    echo "# cctext low-end docker smoke"
    echo "# date=$(date -u +%Y-%m-%dT%H:%M:%SZ)  cpuset=$CPUSET  cpus=$CPUS  memory=$MEMORY"
    echo "# image=$IMAGE"
    echo
    docker run --rm \
        --cpuset-cpus="$CPUSET" \
        --cpus="$CPUS" \
        --memory="$MEMORY" \
        --memory-swap="$MEMORY" \
        --pids-limit=256 \
        -e RTX_TARGETS="${RTX_TARGETS:-}" \
        -e RTX_DUP_ROUNDS="${RTX_DUP_ROUNDS:-}" \
        "$IMAGE" \
        /usr/bin/time -f 'elapsed_sec=%e max_rss_kb=%M' \
        ./make.shcc @smoke
} 2>&1 | tee "$LOG"

echo "docker: log → $LOG" >&2
