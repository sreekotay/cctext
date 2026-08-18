# Perf pins and dated results

- `baseline.env` — regression ceilings for the 8 GiB fixture (`make perf-record`).
- `results/baseline_results_YYYY_MM_DD.txt` — human table from
  `make perf` / `scripts/perf_baseline.sh`: binary sizes, peak RSS per fixture,
  then ops × sizes (ms).

Fixtures (same ops, same byte distance):

- `3M` — mixed prose/code (`testdata/generated/large.txt`, `--bytes 3M`)
- `8G` — same text block (`testdata/generated/large_8G.txt`)
- `2Gjson` — JSON array (`testdata/generated/large_2G.json`) if present

Ops: `open`, `scroll_40`, `wrap_40`, `jump_1m`, `wrap_1m`,
`insert_bof`, `insert_eof`, `insert_mid`, `newline_mid`, `close`.
`wrap_40` fills 40 visual rows from byte 0. `jump_1m` is `line_of(1MiB)`.
`wrap_1m` is `fill_off(1MiB)` (window only; no prefix index).
`insert_mid` / `newline_mid` insert at 1 MiB. Each op is timed on a fresh
`from_path` so a jump cannot cheapen a later insert (the mid insert pays
its own scan-to-offset). `scroll_40` and `jump_1m` include frontier build.
`RTX_PERF_TRIALS` (default 5) repeats each op; the table reports the
minimum ms. RSS is `getrusage` max RSS after the first open and the
process high-water mark (`rss_open` / `rss_peak`). Builds use
`--release` unless `DEBUG=1`. Files above `RTX_LINE_SOFT_MIN` (256 KiB)
stay progressive.
