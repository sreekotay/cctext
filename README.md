# cctext 0.1

A text editor in [Concurrent-C](https://github.com/sreekotay/concurrent-c): one document core, two frontends — **cctext** (POSIX console) and **cctext-ray** (Raylib GPU). Built with Concurrent-C — a strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles with your host C compiler.

This is a standalone app. Building from source needs `ccc` on `PATH` (or `CCC=`). **cctext** also ships as a prebuilt on [GitHub Releases](https://github.com/sreekotay/cctext/releases) (no compiler). It does not live inside the compiler repository.

![cctext TUI on a 2 GiB JSON file — syntax highlight, column select, byte-rail scrollbar](docs/cctext-tui.png)

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

- **Fast on huge files.** An 8 GiB open is 0.006 ms; first-screen scroll is 0.039 ms. The body stays on disk (page store + progressive line index). Numbers, including syntax highlight: [Perf](#perf).
- **Small.** Release `cctext` is 302 KiB. Open RSS is ~1.5 MiB on 3 MiB and on 8 GiB.
- **UTF-8.** Caret, wrap, hit-test, and backspace walk clusters (scalar + combining marks). Hex stays a byte camera.
- **TUI and GUI.** Same document core: **cctext** (POSIX console) and **cctext-ray** (Raylib).
- **Hex.** `Ctrl-L` cycles default → wrap → hex (offset | hex | ASCII).
- **Multiview.** Several files, splits, two cameras on one document. Unlock (`Ctrl-U`) lets a pane scroll off the caret.
- **File browser.** No filename opens it. `o` / `Ctrl-O` opens the built-in browser (glob, walk directories) into the focused view. The GUI uses the system dialog; `Ctrl-Shift-O` keeps the in-app browser there too. A missing path asks to create an empty file.
- **TextMate grammars.** Drop any `.tmLanguage.json` into `grammars/` (or `RTX_GRAMMARS`) — loaded live, no rebuild. Window lex, not a full-file pass.
- **Rectangular selection.** Alt-arrows / Alt-drag is a column box.
- **TUI mouse.** SGR click, drag, and wheel; a byte-rail scrollbar jumps by file offset (not a soft line count). Hit-test is the same layout as the GUI.
- **High-performance scroll.** Only the visible window is measured and highlighted. Idle frames skip relayout.

## Why

- **Piece tree** over a page store (path original + spilled add) plus borrowed buffers. Offset and line lookup are O(log pieces).
- **Sections** set layout policy (code vs prose). **Runs** inside a section are rich text and syntax tokens.
- **Syntax highlight** is a core pass over code sections. Runs carry interned scopes; frontends theme a prefix. Spike lexer, or a lowered TextMate table when the path’s extension is in a grammar loaded at runtime (`RTX_GRAMMARS`, else `<exe>/grammars`, else `<exe>/../testdata/grammars`). Open `testdata/samples/` to try them.
- Measure-generic layout so wrap and hit-test are the same algorithm in pixels (Raylib) and cells (`cctext`). View cycles with `Ctrl-L`: default → wrap → hex (offset|hex|ASCII) → default.
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
./bin/cctext --wrap testdata/wrap.txt --hex testdata/mixed.txt
./bin/cctext-ray --view=hex testdata/generated/large.txt
# ./bin/cctext-ray testdata/generated/large_8G.txt
```

**cctext-ray** has a File / Edit / View / Go menu bar (click a title, then an item). The GUI matches **cctext**: blinking caret, idle skip-layout, unlock scroll. **Esc** still opens the key-binding overlay in both frontends. **g** / **Ctrl-G** jumps to a line (or `N%`). **o** / **Ctrl-O** opens a file in the focused view — the GUI uses the system dialog (live, no restart); **cctext** uses the in-app browser. **Ctrl-Shift-O** keeps that browser in the GUI as a reference (glob, walk directories). **l** / **Ctrl-L** cycles default / wrap / hex. **u** / **Ctrl-U** unlocks the pane from the caret; find and jump still land in the active view. Line numbers are in the gutter. Ctrl/Cmd chords work while the overlay is closed. Unsaved quit asks Save / Don't save / Cancel.

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

See [CCTEXT_PLAN.md](CCTEXT_PLAN.md).

## Perf

Release, best of 5, each op from a fresh `from_path`. Times include syntax highlighting. `jump_1m` / `wrap_1m` / `insert_mid` / `newline_mid` are **1 MiB into every file** so the sizes are comparable. `jump_50pct` is `g` + `50%` (byte mid; scales). 2026-08-19 on Srees-MacBook-Air. Full table: [testdata/perf/results/baseline_results_2026_08_19.txt](testdata/perf/results/baseline_results_2026_08_19.txt). How to re-run: [testdata/perf/README.md](testdata/perf/README.md).

`perf_matrix_smoke` 137.1 KiB · `cctext` 301.8 KiB

| | 3M text | 8G text | 2G JSON |
|---|---:|---:|---:|
| rss_open | 1.5 MiB | 1.5 MiB | 2.1 MiB |
| rss_peak | 3.2 MiB | 55.8 MiB | 27.8 MiB |
| open | 0.005 ms | 0.006 ms | 0.007 ms |
| scroll_40 | 0.039 ms | 0.039 ms | 0.046 ms |
| wrap_40 | 0.050 ms | 0.056 ms | 0.173 ms |
| jump_1m | 0.452 ms | 0.457 ms | 0.399 ms |
| jump_50pct | 0.688 ms | 2197 ms | 454 ms |
| wrap_1m | 0.845 ms | 0.752 ms | 0.829 ms |
| insert_bof | 0.072 ms | 0.090 ms | 0.079 ms |
| insert_eof | 0.066 ms | 0.088 ms | 0.076 ms |
| insert_mid | 0.510 ms | 0.534 ms | 0.456 ms |
| newline_mid | 0.513 ms | 0.525 ms | 0.449 ms |

JSON `rss_open` includes grammar load on the first `.json` path. `wrap_40` there is the TextMate window lex; `wrap_1m` is cheaper because the JSON lines are short. `rss_peak` on 8G / 2G JSON is the half-file `jump_50pct` index.
