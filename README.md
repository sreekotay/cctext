# cctext 0.1

A text editor in [Concurrent-C](https://github.com/sreekotay/concurrent-c) — a strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles with your host C compile.

CCText has one document core, two frontends — **cctext** (POSIX console) and **cctext-ray** (Raylib GPU).

This is a standalone app. Building from source needs `ccc` on `PATH` (or `CCC=`). **cctext** also ships as a prebuilt on [GitHub Releases](https://github.com/sreekotay/cctext/releases) (no compiler). It does not live inside the compiler repository.

![cctext TUI — 2 GiB JSON, syntax highlight, and a box selection](docs/cctext-tui.png)

![cctext file browser — glob, name, size, mtime, and grammar-colored suffixes](docs/cctext-browse.png)

## Getting started

Unpack a [release](https://github.com/sreekotay/cctext/releases) (no compiler) and run it in place:

```bash
tar -xzf cctext-macos-arm64.tar.gz
./cctext-macos-arm64/cctext --version
./cctext-macos-arm64/cctext            # file browser
./cctext-macos-arm64/cctext file.txt   # missing path asks to create
```

From source: `./make.shcc @cctext` then `./bin/cctext` or `./bin/cctext file.txt`. `-v` / `--version` prints `cctext 0.1`.

## Features

- **Fast on huge files.** An 8 GiB open is 0.006 ms; first-screen scroll is 0.044 ms; `g 50%` is 0.002 ms. The body stays on disk (page store + progressive line index). Numbers, including syntax highlight: [Perf](#perf).
- **Small.** Release `cctext` is 459 KiB. Open RSS is ~1.5 MiB on 3 MiB and on 8 GiB.
- **UTF-8.** Caret, wrap, hit-test, and backspace walk clusters (scalar + combining marks / variation selectors). Full UAX #29 graphemes (ZWJ emoji, flags) are pending. Hex stays a byte camera.
- **TUI and GUI.** Same document core: **cctext** (POSIX console) and **cctext-ray** (Raylib).
- **Hex / grid.** `Ctrl-L` cycles default → wrap → hex (offset | hex | UTF-8 dump) → grid (CSV/TSV/pipe columns).
- **Multiview.** Several files, splits, two cameras on one document. Unlock (`Ctrl-U`) lets a pane scroll off the caret.
- **File browser.** No filename opens it. `o` / `Ctrl-O` opens the built-in browser (glob, walk directories) into the focused view. The GUI uses the system dialog; `Ctrl-Shift-O` keeps the in-app browser there too. In the browser, `Ctrl-O` / `Cmd-O` launches this frontend on the selection; `e` / `Ctrl-E` launches the other (`cctext` ↔ `cctext-ray`). A new **cctext** opens in the host terminal (Cursor when you launched from there; iTerm or Terminal.app otherwise). **cctext-ray** is a window, not a terminal. The current folder (not `..`) sizes itself with a pumped walk — the total counts up, pauses if you leave, and resumes when you return. Enter still opens in this instance. A missing path asks to create an empty file.
- **Deep search.** Typing a fragment filters this directory first, then a `> Flattened search` row and nested matches append below (pumped, parallel). `>` skips the local listing and flattens immediately. A fragment is case-insensitive; `*.txt` is a real glob and stays a local listing so you can still walk directories.
- **TextMate grammars.** Drop any `.tmLanguage.json` into `grammars/` (or `RTX_GRAMMARS`) — loaded live, no rebuild. Window lex, not a full-file pass. Shipped: C/CC, JSON, Markdown, CSS, CSV/TSV/pipe, HTML, YAML, shell, Python, JS/TS.
- **Marks and folds.** `Ctrl-K/B` steps highlight marks already in the window (`Ctrl-E/R` for `invalid`). `Ctrl-T` folds a heading or a `{}`/`[]`/`()` pair whose other end is within a page of the caret (256KiB analysis page, plus one neighbor). The matching pair is painted while the caret sits in it. No scan, no AST.
- **TUI mouse.** SGR click, drag, and wheel; a byte-rail scrollbar jumps by file offset (not a soft line count). Hit-test is the same layout as the GUI.
- **Byte jump.** `g 50%` snaps to the mid-file line with a local read (8 KiB). The gutter shows `+1`… / `-L` until the line index catches up; arrows and wheel stay on that camera — they do not `line_of` the prefix. Absolute `g L` pumps the prefix and shows `scanning... N%` on the jump field.
- **High-performance scroll.** Only the visible window is measured and highlighted. Idle frames skip relayout.

## Why

- **Piece tree** over a page store (path original + spilled add) plus borrowed buffers. Offset and line lookup are O(log pieces).
- **Sections** set layout policy (code vs prose). **Runs** inside a section are rich text and syntax tokens.
- **Syntax highlight** is a core pass over code sections. Runs carry interned scopes; frontends theme a prefix. Spike lexer, or a lowered TextMate table when the path’s extension is in a grammar loaded at runtime (`RTX_GRAMMARS`, else `<exe>/grammars`, else `<exe>/../testdata/grammars`). Open `testdata/samples/` to try them.
- Measure-generic layout so wrap and hit-test are the same algorithm in pixels (Raylib) and cells (`cctext`). View cycles with `Ctrl-L`: default → wrap → hex (offset|hex|UTF-8 dump) → grid → default.
- Headless tests do not open a window.

## Status

Piece tree, document, layout, C/CC highlight, save, undo/redo, selection, incremental relayout, and multifile splits. **cctext** and **cctext-ray** share that core.

## Setup

```bash
./setup.sh                  # checks ccc; Raylib is optional for now
./make.shcc @               # list tasks
./make.shcc @smoke          # piece-tree + layout + edit-session + find + large-file
./make.shcc @large          # testdata/generated/large.txt (~3 MiB; LARGE_BYTES= or @large)
make giant                  # testdata/generated/large_8G.txt (~8 GiB; slow)
make giant-json             # testdata/generated/large_2G.json (~2 GiB JSON array; slow)
make giant-smoke            # open that file via page store (mid-line + tiny insert)
make perf                   # ops×sizes + binary/RSS table → testdata/perf/results/…
make perf-record            # that table + refresh testdata/perf/baseline.env pins
make perf-check             # fail if 8G open/scroll/jump/insert regress
./make.shcc @cctext         # console editor (`--release`; DEBUG=1 keeps asserts)
make dist-cctext            # dist/cctext-<os>-<arch>.tar.gz (binary + grammars/)
./make.shcc @cctext_ray     # Raylib GUI (needs `brew install raylib`)
./bin/cctext testdata/mixed.txt testdata/small.txt
./bin/cctext --wrap testdata/wrap.txt --hex testdata/mixed.txt --grid testdata/grid_rfc.csv
./bin/cctext-ray --view=hex testdata/generated/large.txt
# ./bin/cctext-ray testdata/generated/large_8G.txt
```

**cctext-ray** has a File / Edit / View / Go menu bar (click a title, then an item). The GUI matches **cctext**: blinking caret, idle skip-layout, unlock scroll. **Esc** still opens the key-binding overlay in both frontends. **g** / **Ctrl-G** jumps to a line, or `N%` of the file by byte (`+1` / `-L` in the gutter until the index catches up). **o** / **Ctrl-O** opens a file in the focused view — the GUI uses the system dialog (live, no restart); **cctext** uses the in-app browser. **Ctrl-Shift-O** keeps that browser in the GUI as a reference (glob, walk directories). From the browser, **Ctrl-O** / **Cmd-O** starts this app on the selection and **e** / **Ctrl-E** starts the other. **l** / **Ctrl-L** cycles default / wrap / hex / grid. Grid is a columnar paint of the same bytes (CSV/TSV/pipe): widths from this screen, cells wrap. **u** / **Ctrl-U** unlocks the pane from the caret; find and jump still land in the active view. Line numbers are in the gutter. Ctrl/Cmd chords work while the overlay is closed. Unsaved quit asks Save / Don't save / Cancel.

Install `ccc` with Homebrew (`brew tap sreekotay/concurrent-c` / `brew install --HEAD …/ccc`) or from a concurrent-c checkout (`PREFIX=$HOME/.local ./cc-install.sh`). TextMate schema parse uses `<ccc/std/json.cch>` / `include JsonKeep` (closed `TmGrammar` stays in-tree). `#include <raylib.h>` needs a lowerer that seeds passthrough C types (0.3.3-139 or last-good after that).

## Releases

Prebuilt **cctext** only — not cctext-ray. Each GitHub Release attaches:

| Artifact | Host |
|---|---|
| `cctext-linux-x64.tar.gz` | Ubuntu / glibc x86_64 |
| `cctext-macos-arm64.tar.gz` | Apple Silicon |

Unpack and run in place. Grammars load from `./grammars` next to the binary.

```bash
tar -xzf cctext-macos-arm64.tar.gz
./cctext-macos-arm64/cctext file.txt
./cctext-macos-arm64/cctext --wrap file.txt
```

Local tarball (same layout, current machine):

```bash
make dist-cctext            # → dist/cctext-<os>-<arch>.tar.gz
```

Cut a public drop: tag `cctext-v*` and push. CI installs `ccc` from [concurrent-c](https://github.com/sreekotay/concurrent-c), builds `--release`, and attaches both tarballs. `workflow_dispatch` on `.github/workflows/release-cctext.yml` builds artifacts without publishing (optional `ccc_ref` pins the compiler).

```bash
git tag cctext-v0.1.0
git push origin cctext-v0.1.0
```

## Layout

```
core/         document — no raylib.h, no termios
frontend/     Raylib GUI and TTY
tests/        headless smokes
testdata/     small fixtures; generated/ is gitignored
```

See [CCTEXT_PLAN.md](CCTEXT_PLAN.md). MIT — [LICENSE](LICENSE).

## Perf

Release, best of 5, each op from a fresh `from_path`. Times include syntax highlighting. `jump_1m` / `wrap_1m` / `insert_mid` / `newline_mid` are **1 MiB into every file** so the sizes are comparable. `jump_50pct` is `g` + `50%` (`line_floor` + island; no prefix scan). 2026-08-21 on Srees-MacBook-Air. Full table: [testdata/perf/results/baseline_results_2026_08_21.txt](testdata/perf/results/baseline_results_2026_08_21.txt). Prior run: [2026-08-20](testdata/perf/results/baseline_results_2026_08_20.txt). How to re-run: [testdata/perf/README.md](testdata/perf/README.md).

All tests are with syntax hilighting fully active.

`perf_matrix_smoke` 137.7 KiB · `cctext` 458.7 KiB

| | 3M text | 8G text | 2G JSON |
|---|---:|---:|---:|
| rss_open | 1.5 MiB | 1.5 MiB | 3.4 MiB |
| rss_peak | 2.6 MiB | 2.6 MiB | 5.5 MiB |
| open | 0.006 ms | 0.006 ms | 0.006 ms |
| scroll_40 | 0.042 ms | 0.044 ms | 0.055 ms |
| wrap_40 | 0.062 ms | 0.061 ms | 0.174 ms |
| jump_1m | 0.523 ms | 0.524 ms | 0.578 ms |
| jump_50pct | 0.002 ms | 0.002 ms | 0.002 ms |
| wrap_1m | 0.827 ms | 0.818 ms | 0.910 ms |
| insert_bof | 0.085 ms | 0.075 ms | 0.086 ms |
| insert_eof | 0.083 ms | 0.078 ms | 0.086 ms |
| insert_mid | 0.088 ms | 0.083 ms | 0.085 ms |
| newline_mid | 0.073 ms | 0.076 ms | 0.090 ms |

JSON `rss_open` includes grammar load on the first `.json` path. `wrap_40` there is the TextMate window lex; `wrap_1m` is the visible window at 1 MiB. `rss_peak` is the 1 MiB `jump_1m` / mid-insert, not a half-file index.
