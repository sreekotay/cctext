# Perf pins and dated results

- `baseline.env` — regression ceilings for the 8 GiB fixture (`make perf-record`).
- `results/baseline_results_YYYY_MM_DD.txt` — human table of ops × sizes from
  `make perf` / `scripts/perf_baseline.sh`.

Ops: `open`, `scroll_40`, `jump_100k`, `insert_bof`, `insert_eof`,
`insert_mid`, `newline_mid`, `close`.
