# Perf pins and dated results

- `baseline.env` — regression ceilings for the 8 GiB fixture (`make perf-record`).
- `results/baseline_results_YYYY_MM_DD.txt` — human table from
  `make perf` / `scripts/perf_baseline.sh`: binary sizes, peak RSS per fixture,
  then ops × sizes (ms).

Ops: `open`, `scroll_40`, `jump_100k`, `insert_bof`, `insert_eof`,
`insert_mid`, `newline_mid`, `close`. RSS is `getrusage` max RSS after open
and after edits (`rss_open` / `rss_peak`). Builds use `--release` unless
`DEBUG=1`.
