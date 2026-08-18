# Perf pins and dated results

- `baseline.env` — regression ceilings for the 8 GiB fixture (`make perf-record`).
- `results/baseline_results_YYYY_MM_DD.txt` — human table from
  `make perf` / `scripts/perf_baseline.sh`: binary sizes, peak RSS per fixture,
  then ops × sizes (ms).

Ops: `open`, `scroll_40`, `jump_100k`, `insert_bof`, `insert_eof`,
`insert_mid`, `newline_mid`, `close`. Each op is timed on a fresh
`from_path` so a jump cannot cheapen a later insert (the mid insert pays
its own scan-to-offset). `RTX_PERF_TRIALS` (default 5) repeats each op;
the table reports the minimum ms. RSS is `getrusage` max RSS after the
first open and the process high-water mark (`rss_open` / `rss_peak`).
Builds use `--release` unless `DEBUG=1`.
