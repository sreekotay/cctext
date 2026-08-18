# Perf pins and dated results

- `baseline.env` — regression ceilings for the 8 GiB fixture (`make perf-record`).
- `results/baseline_results_YYYY_MM_DD.txt` — human table from
  `make perf` / `scripts/perf_baseline.sh`: binary sizes, peak RSS per fixture,
  then ops × sizes (ms).

Ops: `open`, `scroll_40`, `wrap_40`, `jump_100k`, `wrap_100k`,
`insert_bof`, `insert_eof`, `insert_mid`, `newline_mid`, `close`.
`wrap_40` / `wrap_100k` fill 40 visual rows at width 40 (the generated
prose lines wrap). Each op is timed on a fresh
`from_path` so a jump cannot cheapen a later insert (the mid insert pays
its own scan-to-offset). Line-addressed ops (`scroll_40`, `jump_100k`)
include frontier build; byte-addressed inserts do not unless
`prepare_edit` must scan to the offset. `RTX_PERF_TRIALS` (default 5)
repeats each op; the table reports the minimum ms. `jump_100k` is not
cross-file comparable: the 2 MiB and 8 GiB fixtures have different
bytes/line. RSS is `getrusage` max RSS after the first open and the
process high-water mark (`rss_open` / `rss_peak`). Builds use
`--release` unless `DEBUG=1`. Files above `RTX_LINE_SOFT_MIN` (256 KiB)
stay progressive.
