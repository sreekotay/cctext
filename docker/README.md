# Low-end / single-core Docker smoke

Linux-only (matches CI). Simulates a slow machine with Docker CPU and memory limits.

```bash
./docker/run-smoke.sh
```

Defaults: **cpuset 0** (one real core) plus **1 CPU** CFS quota, **2 GiB RAM**
(enough for `@smoke` including `dup_scale_smoke` at the 1000 MiB checkpoint).
`--cpus=1` alone is quota on a multi-core host and still schedules the
listing wave in parallel; `--cpuset-cpus` is the pin that matches a
one-core box.

Tighter box (smaller dup-scale target):

```bash
RTX_TARGETS=256 MEMORY=768m ./docker/run-smoke.sh
```

Pin the compiler at build time:

```bash
CCC_REF=main ./docker/run-smoke.sh
```

Skip rebuild after the first success:

```bash
SKIP_BUILD=1 ./docker/run-smoke.sh
```

Timing lands in `testdata/generated/docker_smoke.log` (gitignored).

## ASAN / TSAN smokes

Separate from the low-end image above. Headless `@smoke` list only — no GUI/TUI.

Native (Linux or macOS with working clang sanitizers):

```bash
./make.shcc @smoke_asan
./make.shcc @smoke_tsan
# or
./scripts/sanitize_smoke.sh asan
./scripts/sanitize_smoke.sh tsan
```

Defaults `RTX_TARGETS=256` for `dup_scale_smoke` (sanitizer RAM overhead). Override:

```bash
RTX_TARGETS=1000 ./make.shcc @smoke_asan
```

Build artifacts land in `out-asan`/`bin-asan` or `out-tsan`/`bin-tsan`. ASAN and TSAN cannot be combined; run separately.

Docker (Linux, matches CI sanitizer jobs):

```bash
./docker/sanitize-smoke.sh asan
./docker/sanitize-smoke.sh tsan
```

Defaults: **2 CPUs**, **4 GiB RAM**. Logs: `testdata/generated/docker_asan_smoke.log`, `docker_tsan_smoke.log`.

Pin compiler / skip rebuild:

```bash
CCC_REF=main SKIP_BUILD=1 ./docker/sanitize-smoke.sh tsan
```

Image tags: `cctext-asan`, `cctext-tsan` (from `docker/Dockerfile.sanitize` with `SANITIZER=address|thread`).
