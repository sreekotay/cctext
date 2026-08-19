# Raytext

A text editor in [Concurrent-C](https://github.com/sreekotay/concurrent-c): one document core, two frontends — **raytext** (Raylib GPU) and **cctext** (POSIX console).

This is a standalone app. Building from source needs `ccc` on `PATH` (or `CCC=`). **cctext** also ships as a prebuilt on [GitHub Releases](https://github.com/sreekotay/raytext/releases) (no compiler). It does not live inside the compiler repository.

## Why

- **Piece tree** over a page store (path original + spilled add) plus borrowed buffers. Offset and line lookup are O(log pieces).
- **Sections** set layout policy (code vs prose). **Runs** inside a section are rich text and syntax tokens.
- **Syntax highlight** is a core pass over code sections. Runs carry interned scopes; frontends theme a prefix. Spike lexer, or a lowered TextMate table when the path’s extension is in a grammar loaded at runtime (`RTX_GRAMMARS`, else `<exe>/grammars`, else `<exe>/../testdata/grammars`). Open `testdata/samples/` to try them.
- Measure-generic layout so wrap and hit-test are the same algorithm in pixels (Raylib) and cells (`cctext`). View cycles with `Ctrl-L`: default → wrap → hex (offset|hex|ASCII) → default.
- Headless tests do not open a window.

## Status

Piece tree, document, layout, C/CC highlight, save, undo/redo, selection, incremental relayout, and multifile splits. **cctext** and **raytext** share that core.

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
./make.shcc @raytext        # Raylib GUI (needs `brew install raylib`)
./bin/cctext testdata/mixed.txt testdata/small.txt
./bin/cctext --wrap testdata/wrap.txt --hex testdata/mixed.txt
./bin/raytext --view=hex testdata/generated/large.txt
# ./bin/raytext testdata/generated/large_8G.txt
```

**raytext** has a File / Edit / View / Go menu bar (click a title, then an item). The GUI matches **cctext**: blinking caret, idle skip-layout, unlock scroll. **Esc** still opens the key-binding overlay in both frontends. **g** / **Ctrl-G** jumps to a line (or `N%`). **o** / **Ctrl-O** opens a file in the focused view — the GUI uses the system dialog (live, no restart); **cctext** uses the in-app browser. **Ctrl-Shift-O** keeps that browser in the GUI as a reference (glob, walk directories). **l** / **Ctrl-L** cycles default / wrap / hex. **u** / **Ctrl-U** unlocks the pane from the caret; find and jump still land in the active view. Line numbers are in the gutter. Ctrl/Cmd chords work while the overlay is closed. Unsaved quit asks Save / Don't save / Cancel.

Install `ccc` with Homebrew (`brew tap sreekotay/concurrent-c` / `brew install --HEAD …/ccc`) or from a concurrent-c checkout (`PREFIX=$HOME/.local ./cc-install.sh`). TextMate schema parse uses `<ccc/std/json.cch>` / `include JsonKeep` (closed `TmGrammar` stays in-tree). `#include <raylib.h>` needs a lowerer that seeds passthrough C types (0.3.3-139 or last-good after that).

## Releases

Prebuilt **cctext** only — not raytext. Each GitHub Release attaches:

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

See [RAYTEXT_PLAN.md](RAYTEXT_PLAN.md).

## Perf

Release, best of 5, each op from a fresh `from_path`. `jump_1m` / `wrap_1m` / `insert_mid` / `newline_mid` are **1 MiB into every file** so the sizes are comparable. 2026-08-18 on Srees-MacBook-Air. Full table: [testdata/perf/results/baseline_results_2026_08_18.txt](testdata/perf/results/baseline_results_2026_08_18.txt). How to re-run: [testdata/perf/README.md](testdata/perf/README.md).

`perf_matrix_smoke` 119.6 KiB · `cctext` 189.4 KiB

| | 3M text | 8G text | 2G JSON |
|---|---:|---:|---:|
| rss_open | 1.4 MiB | 1.5 MiB | 2.1 MiB |
| rss_peak | 2.8 MiB | 3.0 MiB | 3.3 MiB |
| open | 0.005 ms | 0.006 ms | 0.006 ms |
| scroll_40 | 0.039 ms | 0.038 ms | 0.048 ms |
| wrap_40 | 0.053 ms | 0.034 ms | 0.177 ms |
| jump_1m | 0.456 ms | 0.586 ms | 0.433 ms |
| wrap_1m | 0.761 ms | 0.718 ms | 0.022 ms |
| insert_bof | 0.084 ms | 0.083 ms | 0.080 ms |
| insert_eof | 0.088 ms | 0.087 ms | 0.083 ms |
| insert_mid | 0.529 ms | 0.676 ms | 0.481 ms |
| newline_mid | 0.517 ms | 0.658 ms | 0.478 ms |

JSON `rss_open` includes grammar load on the first `.json` path. `wrap_40` there is the TextMate window lex; `wrap_1m` is cheaper because the JSON lines are short.
