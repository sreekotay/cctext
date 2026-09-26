# Terminal editor head-to-head

Headless, reproducible comparison of **cctext** against terminal editors:
vim, neovim, helix, kakoune, micro, nano, emacs (`-nw`). Each editor runs
in a real pty with a fixed size (200×50). A virtual terminal
([pyte](https://github.com/selectel/pyte)) turns the editor's output into a
screen. Every timing stops when the **screen shows the expected content**, not
when bytes arrive.

```
bench/editors/
  Dockerfile     image with every editor + ccc + cctext (build from repo root)
  editors.py     per-editor argv, clean config, keystrokes (goto/search/quit…)
  run.py         fixture generator + pty/pyte harness + report writer
  results/       <date>.md (tables) + <date>.json (every run, raw) + .log
```

## Running

Natively (editors installed; paths from `editors.py` or `*_BIN` env vars:
`CCTEXT_BIN NVIM_BIN HX_BIN KAK_BIN MICRO_BIN VIM_BIN NANO_BIN EMACS_BIN`):

```bash
pip install pyte==0.8.2
./make.shcc @cctext                                   # release build → bin/cctext
python3 bench/editors/run.py gen --dir /tmp/edbench   # small_c,text_3m,json_100m,json_1g (+ json_2g if asked)
python3 bench/editors/run.py bench --dir /tmp/edbench # all editors × all fixtures present
python3 bench/editors/run.py bench --dir /tmp/edbench --editors cctext,vim --fixtures json_100m --repeats 3
python3 bench/editors/run.py probe helix /tmp/edbench/small.c "b':goto 100\r'"   # print the screen
python3 bench/editors/run.py versions
```

### Re-measuring cctext only (after a cctext change)

The other editors don't change between cctext builds. Rebuild cctext and
re-measure only cctext into an existing results file. `--into` keeps
every other editor's cells, replaces the cctext cells, records the cctext
commit, dirty flag and binary mtime (`meta.builds.cctext`, and `build` on
each cell), and rewrites the matching `.md`:

```bash
CC_CLEAN_TOOL=$PWD/scripts/cclower_root.py ./make.shcc @cctext
python3 bench/editors/run.py bench --editors cctext \
    --into bench/editors/results/2026-09-23.json \
    --dir /path/to/fixtures --fixtures small_c,text_3m,json_100m,json_1g,json_2g
# quick check, one fixture, fewer repeats:
python3 bench/editors/run.py bench --editors cctext --into bench/editors/results/2026-09-23.json \
    --dir /path/to/fixtures --fixtures json_100m --repeats 3
python3 bench/editors/run.py report bench/editors/results/2026-09-23.json   # re-render .md only
```

To avoid overwriting a reference file, copy it first and pass the copy to `--into`.

`--idle SECS` adds an idle step right after open (after the RSS reading).
The editor is left alone for SECS seconds while the harness keeps
draining the pty. The harness records context switches summed over the
process tree and all its threads (`idle_wakeups_s`; a sleeping thread
that wakes costs one switch), CPU time (`idle_cpu_pct`, utime+stime) and
the number of output writes it saw (`idle_writes`). The two idle columns
appear in the tables only for cells that measured them. cctext runs with
`--no-blink`, so this measures the idle loop without a blinking cursor.

The fixtures dir can also be set with `BENCH_FIXTURES`. `--resume` skips
editor×fixture cells already in today's JSON. `--tag` adds a suffix to the
result file names.

Docker (see the header of `Dockerfile`):

```bash
docker build -f bench/editors/Dockerfile -t cctext-editor-bench .
docker run --rm -it -v "$PWD/bench/editors/results:/src/bench/editors/results" \
    -v cctext-bench-fixtures:/fixtures cctext-editor-bench
```

Give Docker at least 12 GiB RAM and ~5 GiB disk for the 1 GiB and 2 GiB JSON cells.

## Fixtures

| name | file | how |
|---|---|---|
| `small_c` | `small.c`, 50 KiB | synthetic C, numbered functions (`run.py gen_small_c`) |
| `text_3m` | `large_3M.txt`, 3 MiB | `testdata/gen_large.sh --bytes 3M` (prose + code blocks; `.txt`, so most editors show no colors) |
| `json_100m` | `large_100M.json` | `gen_large.sh --bytes 100M --json` (one object per line) |
| `json_1g` | `large_1G.json` | `gen_large.sh --bytes 1G --json` |
| `json_2g` | `large_2G.json` | `gen_large.sh --bytes 2G --json` (`make.shcc @giant_json`) |

Every content line carries a record/block number. The harness uses that
number to check where the view is (`"id": N,` for JSON; `block N` / `fn_N` for C and text).

## What one run measures

Each repeat starts the editor fresh with a throwaway `$HOME` / XDG dirs, so
no user config applies. The steps run in order in the same session:

1. **open→paint**: time from `Popen` until the screen shows record 0 **and**
   record 30 (C: block 0 and block 3). This is the first meaningful paint.
   Prompts that appear while opening are answered automatically: emacs
   asks "really open?" above 10 MB and gets `y`. That answer is part of the
   open time.
2. **RSS after open**: `VmRSS` / `VmHWM` from `/proc/<pid>/status`, summed
   over the process tree (nvim runs a TUI plus an `--embed` server). It is
   read once output has been quiet for 300 ms, capped at 3 s.
3. **PageDown ×20** from the top. Each press is timed until the view has
   moved at least half a screen: the largest record number on screen must
   grow by 25 lines' worth of records. The table reports the median per
   page. cctext has no PageDown binding, so one cctext "page" is 24 SGR
   wheel-down events (2 rows each, about one screen) written in one burst.
   Since 4c655a0 cctext coalesces a burst of wheel notches (up to 64)
   into one frame, so the 24-event burst is one repaint. It is still
   wheel input rather than a PageDown key, so read that column with care.
   Builds before 4c655a0 drew one frame per notch (~24 frame times per
   page).
4. **search** for a string about 6 records before EOF. The clock starts
   when the pattern is typed and stops when that record's own line is in
   the document view. A prompt echo or a hit-list row does not count.
   Incremental searches (emacs isearch, micro, cctext find) do their work
   while the pattern is typed, and that work is included. cctext lands with
   Down once its hit list shows the hit.
5. **jump to the middle**. vim/nvim use `50%` (line percent). cctext uses
   Ctrl-G `50%` (byte percent). Editors without a percent goto use their
   goto-line with the middle line: helix `:goto N`, kakoune `Ng`, micro
   `goto N`, nano Ctrl-_, emacs `M-g g`. The clock stops when ≥3 records
   within ±2 % of the middle are on screen.
6. **keystroke latency**: at the mid-file caret (insert mode where modal),
   type `Q` up to 50 times. Each keypress is timed until the screen holds
   one more `Q`, and output is drained between keys. The table reports p50
   and p95. Typing stops early once 20 s of total latency has built up.
   `keys_typed` in the JSON records how many keys were typed.
7. **quit**, discarding the edit (confirmation prompts answered). Timed
   until the process exits.

Per-op timeouts scale with file size: open 20 s + 90 s/GiB, ops 10 s +
60 s/GiB, key 5 s + 5 s/GiB. A step that misses its timeout is recorded as
`TIMEOUT` and ends that session. If the editor's process tree passes
`BENCH_MEM_GUARD_MB` (default 10240), it is killed and recorded as
`OOM-GUARD`. If open fails, the remaining repeats for that editor×fixture
are skipped.

**Host quiet check.** Before each editor×fixture cell the harness waits
(up to 30 min) until the 1-minute load average is below `--max-load`
(default 1.5). The JSON records the load average before and after each
cell (`loadavg_before` / `loadavg_after`).

Repeats: 5 for files < 512 MiB, 3 for larger files (`--repeats`,
`--big-repeats`). The tables show the **median** across repeats. Page
cache is **warm**: each fixture is `cat` to `/dev/null` before its runs.

**Highlighting** is on wherever the editor has it (see `editors.py`,
`highlight`). The `hl colours` column counts the distinct non-default
foreground colours on the content rows after open. 0 or 1 means no
syntax colouring on screen. An editor that turns highlighting off (or
never turns it on) for a huge file is reported as a **feature
difference**, not a speed-up. The JSON also records `colors_at_paint`: a
first paint without colours that gains them later means highlighting ran
asynchronously after the paint (micro does this).

## Configs used

| editor | invocation / config | highlighting |
|---|---|---|
| cctext | `cctext --no-blink FILE` | window lexer + shipped TextMate grammars |
| vim | `vim -u vimrc -n -i NONE` with `syntax on`, `filetype plugin indent on` | stock syntax files ('redrawtime' 2 s, 'synmaxcol' 3000 defaults) |
| nvim | `nvim --clean -n -i NONE` | defaults (regex syntax; bundled tree-sitter is not started for C/JSON by default) |
| helix | `hx -c empty.toml` | default theme, bundled tree-sitter grammars |
| kakoune | `KAKOUNE_CONFIG_DIR=empty kak FILE` | stock kakrc/autoload filetype highlighters |
| micro | `micro -config-dir empty` | default colorscheme + built-in syntax |
| nano | `nano --rcfile nanorc` with `include "/usr/share/nano/*.nanorc"` | nano syntax files |
| emacs | `emacs -nw -Q` (lockfiles/backups/auto-save off) | font-lock (default on), `c-mode` / `js-json-mode` |

TERM is `xterm-256color` and COLORTERM is `truecolor`. The harness
answers DA1/DSR (pyte), secondary DA, and OSC 10/11 colour queries, so
no editor waits on an unanswered terminal query.

## Caveats

- **Measurement granularity.** Times are taken when the harness reads the
  pty chunk that makes the check pass. That includes kernel pty latency
  and the harness's own `select` loop (~0.1 ms). A check can also pass
  before the frame is fully drawn if the editor flushes in several writes.
  Sub-millisecond differences are noise.
- The same keystrokes do different amounts of work in different editors.
  Percent jumps by byte (cctext) and by line (vim) land in slightly
  different places. Goto-line forces a line count in editors that don't
  keep one. Incremental search pays per typed character. These are
  properties of the editors, and they are listed per editor above.
- `.txt` has no grammar in most editors. cctext's markup lens colours prose
  there, so `text_3m` compares colouring cctext does with editors that
  colour nothing.
- `colors` is a heuristic on the visible rows (distinct fg colours), not
  proof that a grammar ran.
- Warm cache only. Cold-cache open (after `drop_caches`) is not measured.
- One machine and one pty size. Absolute numbers depend on CPU and on
  terminal throughput.
- **Shared host.** The 2026-09-23 run shared a 4-core container with other
  agents' builds and sanitizer tests. The load gate waited, but after
  `--patience` it ran anyway. Check `loadavg_before/after` per cell in the
  JSON: the 1 GiB cells ran at load ~2–5. An earlier round was discarded
  because another process ran `pkill -x cctext` in the middle of it.
- nano's keystroke echo is bimodal: some runs paint the typed character in
  ~0.3 ms, others in ~5 ms. The table median can land on either mode.
